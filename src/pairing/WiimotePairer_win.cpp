#include "WiimotePairer.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2bth.h>
#include <windows.h>
#include <bluetoothapis.h>
#include <winioctl.h> // CTL_CODE, FILE_DEVICE_BLUETOOTH (left out by WIN32_LEAN_AND_MEAN)
#include <bthioctl.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace {

/* {00001124-0000-1000-8000-00805F9B34FB}, HumanInterfaceDeviceServiceClass_UUID */
const GUID sHidServiceUuid = {0x00001124, 0x0000, 0x1000, {0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB}};

std::string toUtf8(const WCHAR *str)
{
    const int len = WideCharToMultiByte(CP_UTF8, 0, str, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1)
        return {};
    std::string out(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, str, -1, out.data(), len, nullptr, nullptr);
    return out;
}

/*! \brief Formats an address for display; Windows stores the bytes in reverse display order. */
std::string formatAddress(const BYTE *bytes)
{
    char buf[18];
    std::snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
                  bytes[5], bytes[4], bytes[3], bytes[2], bytes[1], bytes[0]);
    return buf;
}

bool isNintendoDevice(const BLUETOOTH_DEVICE_INFO &info)
{
    return std::wcsncmp(info.szName, L"Nintendo RVL-", 13) == 0;
}

template <typename Callback>
void forEachDevice(HANDLE radio, int inquiryUnits, Callback callback)
{
    BLUETOOTH_DEVICE_SEARCH_PARAMS params = {};
    params.dwSize = sizeof(params);
    params.fReturnAuthenticated = TRUE;
    params.fReturnRemembered = TRUE;
    params.fReturnUnknown = TRUE;
    params.fReturnConnected = TRUE;
    params.fIssueInquiry = inquiryUnits > 0;
    params.cTimeoutMultiplier = static_cast<UCHAR>(inquiryUnits);
    params.hRadio = radio;

    BLUETOOTH_DEVICE_INFO info = {};
    info.dwSize = sizeof(info);
    HBLUETOOTH_DEVICE_FIND find = BluetoothFindFirstDevice(&params, &info);
    if (!find)
        return;
    do {
        if (isNintendoDevice(info))
            callback(info);
        info.dwSize = sizeof(info);
    } while (BluetoothFindNextDevice(find, &info));
    BluetoothFindDeviceClose(find);
}

/*! \brief Answers the legacy PIN request of a pairing attempt with the raw address bytes. */
BOOL CALLBACK onAuthenticationRequest(LPVOID context, PBLUETOOTH_AUTHENTICATION_CALLBACK_PARAMS request)
{
    const BYTE *keyAddress = static_cast<const BYTE *>(context);

    BLUETOOTH_AUTHENTICATE_RESPONSE response = {};
    response.bthAddressRemote = request->deviceInfo.Address;
    response.authMethod = request->authenticationMethod;
    if (request->authenticationMethod == BLUETOOTH_AUTHENTICATION_METHOD_LEGACY) {
        std::memcpy(response.pinInfo.pin, keyAddress, 6);
        response.pinInfo.pinLength = 6;
    } else {
        std::fprintf(stderr, "  unexpected authentication method %d, rejecting\n", request->authenticationMethod);
        response.negativeResponse = TRUE;
    }

    const DWORD result = BluetoothSendAuthenticationResponseEx(nullptr, &response);
    if (result != ERROR_SUCCESS)
        std::fprintf(stderr, "  sending PIN failed (error %lu)\n", result);
    return TRUE;
}

/*! \brief Pairs using the raw address bytes as the legacy PIN.

    The PIN is handed over as raw bytes through an authentication callback. The older
    BluetoothAuthenticateDevice() takes the passkey as a wide string, which Windows converts before sending, so
    address bytes like 0x00 or >= 0x80 end up as a wrong PIN.
 */
bool authenticate(HANDLE radio, BLUETOOTH_DEVICE_INFO &info, const BYTE *keyAddress)
{
    HBLUETOOTH_AUTHENTICATION_REGISTRATION registration = nullptr;
    DWORD result = BluetoothRegisterForAuthenticationEx(&info, &registration, onAuthenticationRequest,
                                                        const_cast<BYTE *>(keyAddress));
    if (result != ERROR_SUCCESS) {
        std::fprintf(stderr, "  registering for authentication failed (error %lu)\n", result);
        return false;
    }
    result = BluetoothAuthenticateDeviceEx(nullptr, radio, &info, nullptr, MITMProtectionNotRequired);
    BluetoothUnregisterAuthentication(registration);
    if (result != ERROR_SUCCESS) {
        std::fprintf(stderr, "  authentication failed (error %lu)\n", result);
        return false;
    }

    // Enumerating the services makes the pairing stick (see Dolphin's IOWin.cpp).
    DWORD serviceCount = 0;
    result = BluetoothEnumerateInstalledServices(radio, &info, &serviceCount, nullptr);
    if (result != ERROR_SUCCESS && result != ERROR_MORE_DATA) {
        std::fprintf(stderr, "  enumerating services failed (error %lu)\n", result);
        return false;
    }
    return true;
}

/*! \brief Checks whether a device is in range and accepting connections, by querying its HID service record.

    Neither the device flags nor stLastSeen tell this apart for paired devices: Windows lists and "sees" them
    regardless. The SDP query opens a real connection, which succeeds within ~1 s for a remote in SYNC or 1+2 mode, and
    fails after the ~5 s page timeout otherwise.
 */
bool isReachable(const BLUETOOTH_DEVICE_INFO &info)
{
    WCHAR address[20];
    const BYTE *bytes = info.Address.rgBytes;
    swprintf_s(address, L"(%02X:%02X:%02X:%02X:%02X:%02X)", bytes[5], bytes[4], bytes[3], bytes[2], bytes[1], bytes[0]);

    WSAQUERYSETW query = {};
    query.dwSize = sizeof(query);
    query.dwNameSpace = NS_BTH;
    query.lpServiceClassId = const_cast<GUID *>(&sHidServiceUuid);
    query.lpszContext = address;

    HANDLE lookup = nullptr;
    if (WSALookupServiceBeginW(&query, LUP_FLUSHCACHE, &lookup) != 0)
        return false;
    WSALookupServiceEnd(lookup);
    return true;
}

} // namespace

WiimotePairer::WiimotePairer() = default;

WiimotePairer::~WiimotePairer()
{
    if (mRadio) {
        CloseHandle(mRadio);
        WSACleanup();
    }
}

bool WiimotePairer::open()
{
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
        return false;

    BLUETOOTH_FIND_RADIO_PARAMS params = {sizeof(params)};
    HANDLE radio = nullptr;
    HBLUETOOTH_RADIO_FIND find = BluetoothFindFirstRadio(&params, &radio);
    if (!find) {
        WSACleanup();
        return false;
    }
    BluetoothFindRadioClose(find);

    BLUETOOTH_RADIO_INFO info = {sizeof(info)};
    if (BluetoothGetRadioInfo(radio, &info) != ERROR_SUCCESS) {
        CloseHandle(radio);
        WSACleanup();
        return false;
    }
    std::memcpy(mHostAddress, info.address.rgBytes, sizeof(mHostAddress));
    mRadio = radio;
    return true;
}

bool WiimotePairer::disconnect(const std::string &address)
{
    // BTH_ADDR holds the address with its first displayed byte as the most significant one.
    BTH_ADDR bthAddress = 0;
    int digits = 0;
    for (char c : address) {
        if (!std::isxdigit(static_cast<unsigned char>(c)))
            continue;
        const int value = std::isdigit(static_cast<unsigned char>(c)) ? c - '0' : std::tolower(c) - 'a' + 10;
        bthAddress = (bthAddress << 4) | static_cast<BTH_ADDR>(value);
        ++digits;
    }
    if (!mRadio || digits != 12)
        return false;

    // Undocumented in the Win32 API docs, but declared in the SDK's bthioctl.h (and used by e.g. DS4Windows).
    DWORD bytesReturned = 0;
    return DeviceIoControl(mRadio, IOCTL_BTH_DISCONNECT_DEVICE, &bthAddress, sizeof(bthAddress), nullptr, 0,
                           &bytesReturned, nullptr) != 0;
}

std::string WiimotePairer::hostAddress() const
{
    return formatAddress(mHostAddress);
}

int WiimotePairer::removeStaleDevices()
{
    int removed = 0;
    forEachDevice(mRadio, 0, [&](BLUETOOTH_DEVICE_INFO &info) {
        if (info.fRemembered && !info.fConnected && !info.fAuthenticated
            && BluetoothRemoveDevice(&info.Address) == ERROR_SUCCESS)
            ++removed;
    });
    return removed;
}

std::vector<WiimotePairer::Device> WiimotePairer::scanAndPair(Method method, int inquiryUnits)
{
    std::vector<Device> paired;
    forEachDevice(mRadio, inquiryUnits, [&](BLUETOOTH_DEVICE_INFO &info) {
        if (info.fConnected)
            return;

        const Device device{toUtf8(info.szName), formatAddress(info.Address.rgBytes)};
        if (info.fAuthenticated) {
            // Paired devices are listed even while off or reconnecting; Windows accepts their reconnection on its
            // own. Only a remote in SYNC or 1+2 mode is reachable here, and it wants a fresh pairing.
            if (!isReachable(info))
                return;
            std::printf("found paired %s [%s] in pairing mode, re-pairing\n", device.name.c_str(),
                        device.address.c_str());
            const DWORD result = BluetoothRemoveDevice(&info.Address);
            if (result != ERROR_SUCCESS) {
                std::fprintf(stderr, "  removing old pairing failed (error %lu)\n", result);
                return;
            }
            info.fAuthenticated = FALSE;
            info.fRemembered = FALSE;
        } else {
            std::printf("found %s [%s]\n", device.name.c_str(), device.address.c_str());
        }

        if (!info.fAuthenticated && method != Method::None) {
            const BYTE *keyAddress = method == Method::SyncButton ? mHostAddress : info.Address.rgBytes;
            if (!authenticate(mRadio, info, keyAddress))
                return;
        }

        const DWORD result = BluetoothSetServiceState(mRadio, &info, &sHidServiceUuid, BLUETOOTH_SERVICE_ENABLE);
        if (result != ERROR_SUCCESS) {
            std::fprintf(stderr, "  enabling HID service failed (error %lu)\n", result);
            return;
        }
        paired.push_back(device);
    });
    return paired;
}
