#include "WiimoteHub.h"

#include <wiiuse.h>

#ifdef _WIN32
#include <hidsdi.h>
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

const int sPlayerLeds[] = {WIIMOTE_LED_1, WIIMOTE_LED_2, WIIMOTE_LED_3, WIIMOTE_LED_4};
const auto sFindInterval = std::chrono::seconds(1);
const auto sShutdownTimeout = std::chrono::seconds(3); //!< for device threads to notice a shutdown
const auto sRumbleDuration = std::chrono::milliseconds(300);
const auto sBlinkDuration = std::chrono::milliseconds(900);
const auto sBlinkPeriod = std::chrono::milliseconds(150);

// What counts as activity for the idle timeout: changes beyond sensor noise.
const float sIdleMotion = 0.15f;      //!< g, per accelerometer axis (a resting remote jitters by ~0.04 g)
const float sIdleStick = 0.1f;        //!< Nunchuk stick, of its -1..1 range
const float sIdleWeightChange = 1.0f; //!< kg, Balance Board total
const float sIdleMinWeight = 2.0f;    //!< kg: someone (or something) on the board counts as activity

int playerLed(int id)
{
    return id < 4 ? sPlayerLeds[id] : WIIMOTE_LED_NONE;
}

DeviceType detectType(const wiimote *wm)
{
    // wiiuse treats the board as a Wiimote with an expansion, which the (synchronous) handshake in wiiuse_find()
    // has already detected. Its INSERTED event is reset by the next poll, so it's never seen.
    if (wm->exp.type == EXP_WII_BOARD)
        return DeviceType::BalanceBoard;
    return wm->type == WIIUSE_WIIMOTE_MOTION_PLUS_INSIDE ? DeviceType::WiimotePlus : DeviceType::Wiimote;
}

/*! \brief Returns the device's Bluetooth address, or an empty string if it can't be determined.

    Bluetooth HID devices report their address as the HID serial number (e.g. "b8ae6e561bbd" on Windows,
    "b8-ae-6e-56-1b-bd" on macOS).
 */
std::string readAddress(const wiimote *wm)
{
#ifdef _WIN32
    wchar_t serial[64] = {};
    if (!HidD_GetSerialNumberString(wm->dev_handle, serial, sizeof(serial)))
        return {};
    std::string ascii;
    for (const wchar_t *c = serial; *c; ++c)
        ascii += static_cast<char>(*c);
    return WiimoteHub::normalizeAddress(ascii);
#elif defined(__APPLE__)
    // Filled by wiiuse's HID backend (wiimosc patch) from the HID serial number, e.g. "8c-cd-e8-c4-d5-0f".
    return WiimoteHub::normalizeAddress(wm->dev_serial);
#else
    (void)wm;
    return {};
#endif
}

/*! \brief Connection feedback: rumble for Wiimotes, a blinking power LED for the Balance Board. */
class Feedback
{
public:
    Feedback(wiimote *wm, int id, DeviceType type)
        : mWiimote(wm)
        , mIsBoard(type == DeviceType::BalanceBoard)
        , mStart(Clock::now())
    {
        if (!mIsBoard) {
            wiiuse_set_leds(wm, playerLed(id));
            wiiuse_rumble(wm, 1);
        }
    }

    void update()
    {
        if (!mIsActive)
            return;

        const auto elapsed = Clock::now() - mStart;
        if (!mIsBoard) {
            if (elapsed >= sRumbleDuration) {
                wiiuse_rumble(mWiimote, 0);
                mIsActive = false;
            }
            return;
        }

        // The Balance Board has no rumble; its power button LED is LED 1.
        const bool isLedOn = elapsed >= sBlinkDuration || (elapsed / sBlinkPeriod) % 2 == 1;
        if (isLedOn != mIsLedOn) {
            wiiuse_set_leds(mWiimote, isLedOn ? WIIMOTE_LED_1 : WIIMOTE_LED_NONE);
            mIsLedOn = isLedOn;
        }
        if (elapsed >= sBlinkDuration)
            mIsActive = false;
    }

private:
    wiimote *mWiimote;
    bool mIsBoard;
    bool mIsActive = true;
    bool mIsLedOn = false;
    Clock::time_point mStart;
};

/*! \brief Tracks how long a device has gone unused, for the idle timeout.

    Activity is a button change (Wiimote, Nunchuk, board), motion beyond sensor noise (only if a listener enabled
    motion sensing; otherwise the accelerometer values stay 0), a moved Nunchuk stick, a changed weight on the
    Balance Board, or anything heavier than sIdleMinWeight standing on it. Changes are measured against the state
    at the last activity, so slow drift also adds up to activity eventually.
 */
class IdleWatcher
{
public:
    explicit IdleWatcher(const wiimote *wm)
        : mWiimote(wm)
    {
        reset();
    }

    /*! \brief Checks the device's state, and returns how long it has been idle. */
    Clock::duration update()
    {
        if (isActive())
            reset();
        return Clock::now() - mLastActivity;
    }

    /*! \brief Counts the current state as activity. */
    void reset()
    {
        const wiimote *wm = mWiimote;
        mLastActivity = Clock::now();
        mExpansion = wm->exp.type;
        mButtons = wm->btns;
        mMotion = wm->gforce;
        if (wm->exp.type == EXP_NUNCHUK) {
            mChuckButtons = wm->exp.nunchuk.btns;
            mChuckMotion = wm->exp.nunchuk.gforce;
            mStickX = wm->exp.nunchuk.js.x;
            mStickY = wm->exp.nunchuk.js.y;
        } else if (wm->exp.type == EXP_WII_BOARD) {
            mWeight = boardWeight();
        }
    }

private:
    static bool hasMoved(const gforce_t &a, const gforce_t &b)
    {
        return std::abs(a.x - b.x) > sIdleMotion || std::abs(a.y - b.y) > sIdleMotion || std::abs(a.z - b.z) > sIdleMotion;
    }

    float boardWeight() const
    {
        const wii_board_t &board = mWiimote->exp.wb;
        return board.tl + board.tr + board.bl + board.br;
    }

    bool isActive() const
    {
        const wiimote *wm = mWiimote;
        if (wm->exp.type != mExpansion || wm->btns != mButtons || hasMoved(wm->gforce, mMotion))
            return true;
        if (wm->exp.type == EXP_NUNCHUK) {
            const nunchuk_t &chuck = wm->exp.nunchuk;
            return chuck.btns != mChuckButtons || hasMoved(chuck.gforce, mChuckMotion)
                || std::abs(chuck.js.x - mStickX) > sIdleStick || std::abs(chuck.js.y - mStickY) > sIdleStick;
        }
        if (wm->exp.type == EXP_WII_BOARD) {
            const float weight = boardWeight();
            return weight >= sIdleMinWeight || std::abs(weight - mWeight) >= sIdleWeightChange;
        }
        return false;
    }

    const wiimote *mWiimote;
    Clock::time_point mLastActivity;
    int mExpansion = EXP_NONE;
    int mButtons = 0;
    gforce_t mMotion = {};
    int mChuckButtons = 0;
    gforce_t mChuckMotion = {};
    float mStickX = 0.0f;
    float mStickY = 0.0f;
    float mWeight = 0.0f;
};

} // namespace

const char *deviceTypeName(DeviceType type)
{
    switch (type) {
    case DeviceType::Wiimote:
        return "Wiimote";
    case DeviceType::WiimotePlus:
        return "Wiimote Plus";
    case DeviceType::BalanceBoard:
        return "Balance Board";
    }
    return "?";
}

WiimoteHub::WiimoteHub(AbstractHubListener *listener)
    : mListener(listener)
{
}

WiimoteHub::~WiimoteHub()
{
    stop();
    if (mWiimotes)
        wiiuse_cleanup(mWiimotes, sMaxDevices);
    if (mPlaceholders)
        wiiuse_cleanup(mPlaceholders, sMaxDevices);
}

std::string WiimoteHub::normalizeAddress(const std::string &address)
{
    std::string digits;
    for (char c : address) {
        if (std::isxdigit(static_cast<unsigned char>(c)))
            digits += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        else if (c != ':' && c != '-')
            return {};
    }
    if (digits.size() != 12)
        return {};

    std::string out;
    for (size_t i = 0; i < digits.size(); i += 2) {
        if (!out.empty())
            out += ':';
        out += digits.substr(i, 2);
    }
    return out;
}

bool WiimoteHub::parseFixedId(const std::string &argument, std::string *error)
{
    const size_t separator = argument.find('=');
    const std::string idText = argument.substr(0, separator);
    if (separator == std::string::npos || idText.empty()
        || !std::all_of(idText.begin(), idText.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
        *error = "expected <id>=<address>, e.g. 0=B8:AE:6E:56:1B:BD, got \"" + argument + "\"";
        return false;
    }
    return setFixedId(std::stoi(idText), argument.substr(separator + 1), error);
}

bool WiimoteHub::setFixedId(int id, const std::string &address, std::string *error)
{
    const std::string normalized = normalizeAddress(address);
    if (normalized.empty()) {
        *error = "invalid Bluetooth address \"" + address + "\"";
        return false;
    }
    if (id < 0 || id >= sMaxDevices) {
        *error = "id " + std::to_string(id) + " out of range (0-" + std::to_string(sMaxDevices - 1) + ")";
        return false;
    }
    for (const auto &fixed : mFixedIds) {
        if (fixed.first == normalized) {
            *error = normalized + " has more than one id";
            return false;
        }
        if (fixed.second == id) {
            *error = "id " + std::to_string(id) + " is assigned more than once";
            return false;
        }
    }
    mFixedIds[normalized] = id;
    return true;
}

bool WiimoteHub::start(WiimotePairer::Method method)
{
    auto pairer = std::make_unique<WiimotePairer>();
    if (!pairer->open()) {
        std::fprintf(stderr, "no Bluetooth radio found\n");
        return false;
    }
    std::printf("radio %s\n", pairer->hostAddress().c_str());
    std::printf("removed %d stale device(s)\n", pairer->removeStaleDevices());

    // Detached rather than joined, so shutting down doesn't wait for a running inquiry. The thread only touches
    // its own pairer and the shared flag.
    *mIsPairing = true;
    std::thread([pairer = std::move(pairer), method, isPairing = mIsPairing, isPaused = mIsPairingPaused]() {
        while (*isPairing) {
            if (*isPaused) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            for (const auto &device : pairer->scanAndPair(method))
                std::printf("paired %s [%s]\n", device.name.c_str(), device.address.c_str());
        }
    }).detach();

    mWiimotes = wiiuse_init(sMaxDevices);
    mPlaceholders = wiiuse_init(sMaxDevices);
    wiiuse_set_output(LOGLEVEL_INFO, nullptr);

    mIsRunning = true;
    mFinderThread = std::thread(&WiimoteHub::runFinder, this);
    return true;
}

void WiimoteHub::stop()
{
    if (mIsRunning)
        printConnectedDevices();

    *mIsPairing = false;
    if (mIsRunning && mIsShuttingDownOnExit)
        shutDownDevices();
    mIsRunning = false;
    if (mFinderThread.joinable())
        mFinderThread.join();
    for (std::thread &thread : mDeviceThreads) {
        if (thread.joinable())
            thread.join();
    }
}

void WiimoteHub::printConnectedDevices()
{
    std::vector<Slot> devices;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        for (const Slot &slot : mSlots) {
            if (slot.isBusy)
                devices.push_back(slot);
        }
    }
    if (devices.empty()) {
        std::printf("no devices connected\n");
        return;
    }
    std::sort(devices.begin(), devices.end(), [](const Slot &a, const Slot &b) { return a.id < b.id; });

    std::string arguments;
    std::printf("connected devices:\n");
    for (const Slot &device : devices) {
        std::printf("  [%d] %-13s %s\n", device.id, deviceTypeName(device.type),
                    device.address.empty() ? "(unknown address)" : device.address.c_str());
        if (!device.address.empty())
            arguments += (arguments.empty() ? "" : " ") + ("--id " + std::to_string(device.id) + "=" + device.address);
    }
    if (!arguments.empty())
        std::printf("to keep these ids: %s\n", arguments.c_str());
}

void WiimoteHub::shutDownDevices()
{
    std::vector<Slot> devices;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        for (const Slot &slot : mSlots) {
            if (slot.isBusy)
                devices.push_back(slot);
        }
    }
    if (devices.empty())
        return;

    // The pairing thread owns its pairer; a second one for the radio handle is fine.
    WiimotePairer pairer;
    if (!pairer.open()) {
        std::fprintf(stderr, "shutdown: no Bluetooth radio found\n");
        return;
    }
    for (const Slot &device : devices) {
        if (device.address.empty()) {
            std::printf("[%d] %s: can't shut down, unknown address\n", device.id, deviceTypeName(device.type));
        } else if (!pairer.disconnect(device.address)) {
            std::printf("[%d] %s %s: shutting down failed\n", device.id, deviceTypeName(device.type),
                        device.address.c_str());
        } else {
            std::printf("[%d] %s %s: shutting down\n", device.id, deviceTypeName(device.type),
                        device.address.c_str());
        }
    }

    // Let the device threads notice the disconnection, so listeners hear about it.
    const auto deadline = Clock::now() + sShutdownTimeout;
    while (Clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> lock(mMutex);
            if (std::none_of(std::begin(mSlots), std::end(mSlots), [](const Slot &slot) { return slot.isBusy; }))
                return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

int WiimoteHub::chooseId(const std::string &address) const
{
    auto isUsed = [this](int id) {
        return std::any_of(std::begin(mSlots), std::end(mSlots),
                           [id](const Slot &slot) { return slot.isBusy && slot.id == id; });
    };
    auto isReserved = [this](int id) {
        return std::any_of(mFixedIds.begin(), mFixedIds.end(), [id](const auto &fixed) { return fixed.second == id; });
    };

    const auto fixed = mFixedIds.find(address);
    if (fixed != mFixedIds.end() && !isUsed(fixed->second))
        return fixed->second;
    // Keep ids of fixed devices free while they're away, unless all others are in use.
    for (int id = 0; id < sMaxDevices; ++id) {
        if (!isUsed(id) && !isReserved(id))
            return id;
    }
    for (int id = 0; id < sMaxDevices; ++id) {
        if (!isUsed(id))
            return id;
    }
    return -1; // unreachable: there are as many ids as slots
}

void WiimoteHub::runFinder()
{
    while (mIsRunning) {
        // wiiuse_find() fills slots that aren't connected, and skips HID devices that a connected slot has open
        // (wiimosc patch). Busy slots are owned by their device thread, so find gets connected placeholders with
        // the same device paths instead. It gets a single free slot, so it returns after one device's (slow,
        // synchronous) handshake, and that device starts streaming without waiting for the others.
        wiimote *slots[sMaxDevices + 1];
        int freeIndex = -1;
        int count = 0;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            for (int i = 0; i < sMaxDevices; ++i) {
                if (!mSlots[i].isBusy) {
                    if (freeIndex < 0) {
                        freeIndex = i;
                        slots[count++] = mWiimotes[i];
                    }
                    continue;
                }
                wiimote *placeholder = mPlaceholders[i];
                placeholder->state = WIIMOTE_STATE_CONNECTED;
                std::memcpy(placeholder->dev_path, mWiimotes[i]->dev_path, sizeof(placeholder->dev_path));
#ifdef __APPLE__
                std::memcpy(placeholder->dev_serial, mWiimotes[i]->dev_serial, sizeof(placeholder->dev_serial));
#endif
                slots[count++] = placeholder;
            }
        }

        if (freeIndex >= 0 && wiiuse_find(slots, count, 0) > 0) {
            if (mDeviceThreads[freeIndex].joinable())
                mDeviceThreads[freeIndex].join(); // the previous device in this slot has finished

            wiimote *wm = mWiimotes[freeIndex];
            Slot slot;
            slot.isBusy = true;
            slot.type = detectType(wm);
            slot.address = readAddress(wm);
            {
                std::lock_guard<std::mutex> lock(mMutex);
                slot.id = chooseId(slot.address);
                mSlots[freeIndex] = slot;
            }
            mDeviceThreads[freeIndex] =
                std::thread(&WiimoteHub::runDevice, this, freeIndex, slot.id, slot.type, slot.address);
            continue; // look for further devices right away
        }

        const auto nextFind = Clock::now() + sFindInterval;
        while (mIsRunning && Clock::now() < nextFind)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

void WiimoteHub::runDevice(int index, int id, DeviceType type, std::string address)
{
    wiimote *wm = mWiimotes[index];
    const char *printedAddress = address.empty() ? "(unknown address)" : address.c_str();

    std::printf("[%d] %s %s connected\n", id, deviceTypeName(type), printedAddress);
    Feedback feedback(wm, id, type);
    if (mListener)
        mListener->connected(id, wm, type);
    IdleWatcher idleWatcher(wm);

    // Polling a single device: its read timeout (~10 ms) paces the loop.
    wiimote *polled[] = {wm};
    while (mIsRunning && WIIMOTE_IS_CONNECTED(wm)) {
        if (wiiuse_poll(polled, 1) && wm->event != WIIUSE_NONE && mListener)
            mListener->event(id, wm);
        if (!WIIMOTE_IS_CONNECTED(wm))
            break;
        feedback.update();
        if (mListener)
            mListener->update(id, wm);

        if (mIdleTimeout.count() > 0 && idleWatcher.update() >= mIdleTimeout) {
            // Like a Wii does: drop the connection, which switches the device off. This thread then sees the
            // disconnection as usual. On failure, try again after another timeout.
            std::printf("[%d] %s %s idle for %g min, shutting down\n", id, deviceTypeName(type), printedAddress,
                        std::chrono::duration<double, std::ratio<60>>(mIdleTimeout).count());
            WiimotePairer pairer;
            if (address.empty() || !pairer.open() || !pairer.disconnect(address))
                std::printf("[%d] %s %s: shutting down failed\n", id, deviceTypeName(type), printedAddress);
            idleWatcher.reset();
        }
    }

    // Stopping while the remote rumbles (e.g. on request, wiimosc-stream's mote/rumble) would leave it rumbling.
    if (WIIMOTE_IS_CONNECTED(wm) && WIIMOTE_IS_SET(wm, WIIMOTE_STATE_RUMBLE))
        wiiuse_rumble(wm, 0);

    if (!WIIMOTE_IS_CONNECTED(wm)) {
        std::printf("[%d] %s %s disconnected\n", id, deviceTypeName(type), printedAddress);
        if (mListener)
            mListener->disconnected(id, type);
        wiiuse_disconnect(wm); // closes the device handle
    }

    std::lock_guard<std::mutex> lock(mMutex);
    mSlots[index] = Slot{};
}
