#include "WiimotePairer.h"

#include <gio/gio.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

namespace {

const char *sService = "org.bluez";
const char *sAdapterInterface = "org.bluez.Adapter1";
const char *sDeviceInterface = "org.bluez.Device1";
const char *sPropertiesInterface = "org.freedesktop.DBus.Properties";
const int sCallTimeout = 10000;    //!< ms

/*! \brief Owns a GVariant reference. */
class Variant
{
public:
    Variant() = default;
    explicit Variant(GVariant *variant)
        : mVariant(variant)
    {
    }
    ~Variant()
    {
        if (mVariant)
            g_variant_unref(mVariant);
    }
    Variant(const Variant &) = delete;
    Variant &operator=(const Variant &) = delete;

    GVariant *get() const { return mVariant; }
    explicit operator bool() const { return mVariant != nullptr; }

private:
    GVariant *mVariant = nullptr;
};

/*! \brief The BlueZ connection of a WiimotePairer: the system bus and the first adapter. */
struct Radio {
    GDBusConnection *connection = nullptr;
    std::string adapterPath;
};

/*! \brief A device BlueZ knows about, from the object manager. */
struct BluezDevice {
    std::string path;
    std::string address;
    std::string name;
    bool isPaired = false;
    bool isConnected = false;
    bool hasRssi = false; //!< only set while the device is being seen by an inquiry
    unsigned int deviceClass = 0;

    /*! \brief Checks the name or, while BlueZ doesn't know it (it often doesn't), the class of device.

        Peripheral (major 5) with minor class joystick (Wiimote, Balance Board) or gamepad (Wiimote Plus).
     */
    bool isNintendo() const
    {
        if (name.rfind("Nintendo RVL-", 0) == 0)
            return true;
        const unsigned int minor = (deviceClass >> 2) & 0x0f;
        return name.empty() && ((deviceClass >> 8) & 0x1f) == 0x05 && (minor == 1 || minor == 2);
    }
};

/*! \brief BlueZ names a device's object after its address, with underscores. */
std::string devicePath(const Radio &radio, const std::string &address)
{
    std::string name = address;
    std::replace(name.begin(), name.end(), ':', '_');
    return radio.adapterPath + "/dev_" + name;
}

void printError(const char *what, GError *error)
{
    std::fprintf(stderr, "  %s failed: %s\n", what, error ? error->message : "unknown error");
}

/*! \brief Calls a BlueZ method; returns an empty Variant (and prints the error, unless \a isQuiet) on failure. */
Variant call(const Radio &radio, const std::string &path, const char *interface, const char *method,
             GVariant *parameters, int timeout = sCallTimeout, bool isQuiet = false)
{
    GError *error = nullptr;
    GVariant *result = g_dbus_connection_call_sync(radio.connection, sService, path.c_str(), interface, method,
                                                   parameters, nullptr, G_DBUS_CALL_FLAGS_NONE, timeout, nullptr,
                                                   &error);
    if (!result && !isQuiet)
        printError(method, error);
    if (error)
        g_error_free(error);
    return Variant(result);
}

bool setProperty(const Radio &radio, const std::string &path, const char *interface, const char *property,
                 GVariant *value)
{
    return static_cast<bool>(call(radio, path, sPropertiesInterface, "Set",
                                  g_variant_new("(ssv)", interface, property, value)));
}

std::string stringProperty(GVariant *properties, const char *key)
{
    std::string text;
    GVariant *value = g_variant_lookup_value(properties, key, G_VARIANT_TYPE_STRING);
    if (value) {
        text = g_variant_get_string(value, nullptr);
        g_variant_unref(value);
    }
    return text;
}

bool boolProperty(GVariant *properties, const char *key)
{
    bool result = false;
    GVariant *value = g_variant_lookup_value(properties, key, G_VARIANT_TYPE_BOOLEAN);
    if (value) {
        result = g_variant_get_boolean(value);
        g_variant_unref(value);
    }
    return result;
}

unsigned int uintProperty(GVariant *properties, const char *key)
{
    unsigned int result = 0;
    GVariant *value = g_variant_lookup_value(properties, key, G_VARIANT_TYPE_UINT32);
    if (value) {
        result = g_variant_get_uint32(value);
        g_variant_unref(value);
    }
    return result;
}

bool hasProperty(GVariant *properties, const char *key)
{
    GVariant *value = g_variant_lookup_value(properties, key, nullptr);
    if (value)
        g_variant_unref(value);
    return value != nullptr;
}

/*! \brief Calls \a visit(path, interfaces) for every object BlueZ exports; stops when it returns true. */
template<typename Visitor>
bool forEachObject(const Radio &radio, Visitor visit)
{
    Variant objects = call(radio, "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", nullptr,
                           sCallTimeout, true);
    if (!objects)
        return false;
    GVariant *dictionary = g_variant_get_child_value(objects.get(), 0);
    GVariantIter iter;
    g_variant_iter_init(&iter, dictionary);
    const char *path;
    GVariant *interfaces;
    bool isStopped = false;
    while (!isStopped && g_variant_iter_next(&iter, "{&o@a{sa{sv}}}", &path, &interfaces)) {
        isStopped = visit(path, interfaces);
        g_variant_unref(interfaces);
    }
    g_variant_unref(dictionary);
    return isStopped;
}

/*! \brief Lists the adapter's Nintendo devices (see BluezDevice::isNintendo()). */
std::vector<BluezDevice> nintendoDevices(const Radio &radio)
{
    std::vector<BluezDevice> devices;
    forEachObject(radio, [&](const char *path, GVariant *interfaces) {
        GVariant *properties = g_variant_lookup_value(interfaces, sDeviceInterface, G_VARIANT_TYPE("a{sv}"));
        if (!properties)
            return false;
        BluezDevice device;
        device.path = path;
        device.address = stringProperty(properties, "Address");
        device.name = stringProperty(properties, "Name");
        device.isPaired = boolProperty(properties, "Paired");
        device.isConnected = boolProperty(properties, "Connected");
        device.hasRssi = hasProperty(properties, "RSSI");
        device.deviceClass = uintProperty(properties, "Class");
        g_variant_unref(properties);
        if (device.isNintendo() && device.path.rfind(radio.adapterPath + "/", 0) == 0)
            devices.push_back(device);
        return false;
    });
    return devices;
}

void startDiscovery(const Radio &radio)
{
    // Wiimotes are Bluetooth Classic devices; skipping LE shortens the inquiry
    GVariantBuilder filter;
    g_variant_builder_init(&filter, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&filter, "{sv}", "Transport", g_variant_new_string("bredr"));
    call(radio, radio.adapterPath, sAdapterInterface, "SetDiscoveryFilter", g_variant_new("(a{sv})", &filter),
         sCallTimeout, true);
    call(radio, radio.adapterPath, sAdapterInterface, "StartDiscovery", nullptr, sCallTimeout, true);
}

void stopDiscovery(const Radio &radio)
{
    call(radio, radio.adapterPath, sAdapterInterface, "StopDiscovery", nullptr, sCallTimeout, true);
}

} // namespace

WiimotePairer::WiimotePairer() = default;

WiimotePairer::~WiimotePairer()
{
    Radio *radio = static_cast<Radio *>(mRadio);
    if (radio && radio->connection)
        g_object_unref(radio->connection);
    delete radio;
}

bool WiimotePairer::open()
{
    if (mRadio)
        return true;

    GError *error = nullptr;
    GDBusConnection *connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &error);
    if (!connection) {
        printError("connecting to the system bus", error);
        g_error_free(error);
        return false;
    }

    Radio *radio = new Radio;
    radio->connection = connection;
    forEachObject(*radio, [&](const char *path, GVariant *interfaces) {
        GVariant *properties = g_variant_lookup_value(interfaces, sAdapterInterface, G_VARIANT_TYPE("a{sv}"));
        if (!properties)
            return false;
        radio->adapterPath = path;
        const std::string address = stringProperty(properties, "Address");
        g_variant_unref(properties);
        unsigned int bytes[6] = {};
        if (std::sscanf(address.c_str(), "%x:%x:%x:%x:%x:%x", &bytes[0], &bytes[1], &bytes[2], &bytes[3], &bytes[4],
                        &bytes[5]) == 6) {
            for (int i = 0; i < 6; ++i)
                mHostAddress[i] = static_cast<unsigned char>(bytes[i]);
        }
        return true;
    });
    if (radio->adapterPath.empty()) {
        std::fprintf(stderr, "no Bluetooth adapter found (is bluetoothd running?)\n");
        g_object_unref(connection);
        delete radio;
        return false;
    }
    mRadio = radio;

    if (!setProperty(*radio, radio->adapterPath, sAdapterInterface, "Powered", g_variant_new_boolean(TRUE)))
        std::fprintf(stderr, "  powering the adapter on failed\n");
    return true;
}

bool WiimotePairer::disconnect(const std::string &address)
{
    Radio *radio = static_cast<Radio *>(mRadio);
    return radio && call(*radio, devicePath(*radio, address), sDeviceInterface, "Disconnect", nullptr);
}

void WiimotePairer::processEvents(int milliseconds)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

std::string WiimotePairer::hostAddress() const
{
    char buf[18];
    std::snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", mHostAddress[0], mHostAddress[1],
                  mHostAddress[2], mHostAddress[3], mHostAddress[4], mHostAddress[5]);
    return buf;
}

int WiimotePairer::removeStaleDevices()
{
    // A remote that bluetoothd has paired reconnects through bluetoothd on a button press, and the kernel's
    // hid-wiimote driver takes it over. Unpairing leaves wiimosc the only one connecting to it.
    Radio *radio = static_cast<Radio *>(mRadio);
    int removed = 0;
    if (!radio)
        return removed;
    for (const BluezDevice &device : nintendoDevices(*radio)) {
        if (device.isPaired
            && call(*radio, radio->adapterPath, sAdapterInterface, "RemoveDevice",
                    g_variant_new("(o)", device.path.c_str())))
            ++removed;
    }
    return removed;
}

std::vector<WiimotePairer::Device> WiimotePairer::scanAndPair(Method method, int inquiryUnits)
{
    std::vector<Device> found;
    Radio *radio = static_cast<Radio *>(mRadio);
    if (!radio)
        return found;

    (void)method; // nothing is paired: both SYNC and 1+2 remotes accept an unauthenticated connection

    startDiscovery(*radio);
    processEvents(static_cast<int>(inquiryUnits * 1280));
    const std::vector<BluezDevice> devices = nintendoDevices(*radio);
    stopDiscovery(*radio);

    for (const BluezDevice &device : devices) {
        // Only a remote in pairing mode answers the inquiry (RSSI is set). Connected ones are already ours.
        if (device.isConnected || !device.hasRssi)
            continue;
        Device info;
        info.name = device.name.empty() ? "Nintendo device (name unknown)" : device.name;
        info.address = device.address;
        info.isMotionPlusInside = ((device.deviceClass >> 2) & 0x0f) == 2; // gamepad: RVL-CNT-01-TR
        found.push_back(info);
    }
    return found;
}
