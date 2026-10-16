#pragma once

#include "pairing/WiimotePairer.h"

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

struct wiimote_t;

enum class DeviceType {
    Wiimote,
    WiimotePlus, //!< Wiimote with built-in MotionPlus (RVL-CNT-01-TR)
    BalanceBoard
};

const char *deviceTypeName(DeviceType type);

/*! \brief Receives connection changes and input from a WiimoteHub.

    Each device runs on its own thread, and all calls for an \a id come from that thread: calls for one id are never
    concurrent, but calls for different ids are. \a id is 0-based and below WiimoteHub::sMaxDevices; ids 0-3 light
    player LEDs 1-4. \a wm stays valid until disconnected() returns.
 */
class AbstractHubListener
{
public:
    virtual ~AbstractHubListener() = default;

    /*! \brief A device connected, or was already connected when the hub started. */
    virtual void connected(int id, wiimote_t *wm, DeviceType type) {}
    virtual void disconnected(int id, DeviceType type) {}

    /*! \brief Called for each wiiuse event of a connected device (input, status, expansion changes, ...). */
    virtual void event(int id, wiimote_t *wm) {}

    /*! \brief Called after each poll of a connected device (roughly every 10 ms, or per input report). */
    virtual void update(int id, wiimote_t *wm) {}
};

/*! \brief Pairs devices in the background, keeps track of connected devices, and gives connection feedback.

    wiiuse blocks in many places (connection handshake, expansion handshake, synchronous reads), for up to several
    seconds. To keep that from stalling other devices, everything runs on separate threads: one for pairing, one
    that picks up newly connected HID devices (wiiuse_find), and one per connected device. New connections are
    reported on stdout, and the device rumbles (Wiimote) or blinks its power LED (Balance Board).

    Ids belong to devices, identified by their Bluetooth address: devices with a fixed id (setFixedId()) always get
    it, all others get the lowest id that is neither in use nor reserved for a fixed device.
 */
class WiimoteHub
{
public:
    static const int sMaxDevices = 8;

    explicit WiimoteHub(AbstractHubListener *listener = nullptr);
    ~WiimoteHub();

    WiimoteHub(const WiimoteHub &) = delete;
    WiimoteHub &operator=(const WiimoteHub &) = delete;

    /*! \brief Formats a Bluetooth address as "XX:XX:XX:XX:XX:XX".

        Accepts any separators and case ("b8ae6e561bbd", "B8-AE-..."); returns an empty string if \a address doesn't
        contain exactly 12 hex digits.
     */
    static std::string normalizeAddress(const std::string &address);

    /*! \brief Parses an "--id" argument of the form "<id>=<address>", and calls setFixedId(). */
    bool parseFixedId(const std::string &argument, std::string *error);

    /*! \brief Always assigns \a id to the device with \a address. Must be called before start(). */
    bool setFixedId(int id, const std::string &address, std::string *error);

    /*! \brief Makes stop() switch off all connected devices, by dropping their Bluetooth connections. */
    void setShutdownOnExit(bool isEnabled) { mIsShuttingDownOnExit = isEnabled; }

    /*! \brief Switches off devices that have been idle for \a minutes (0 = never). Must be called before start().

        Idle means no button changes, no motion beyond sensor noise (motion only counts if a listener enabled motion
        sensing), no Nunchuk stick movement, and an empty, unchanged Balance Board. Devices are switched off by
        dropping their Bluetooth connections; listeners get disconnected() as usual.
     */
    void setIdleTimeout(double minutes)
    {
        mIdleTimeout = std::chrono::milliseconds(static_cast<long long>(minutes * 60000.0));
    }

    /*! \brief Opens the Bluetooth radio, and starts pairing and picking up devices in the background. */
    bool start(WiimotePairer::Method method);

    /*! \brief Prints the connected devices (as "--id" arguments, to keep their ids), stops picking up devices, and
        waits for the device threads to finish.

        With setShutdownOnExit(), the devices are disconnected (and so switched off) first, and their threads report
        it as usual (listeners get disconnected()).
     */
    void stop();

private:
    struct Slot {
        bool isBusy = false; //!< owned by a device thread
        int id = -1;
        DeviceType type = DeviceType::Wiimote;
        std::string address;
    };

    void runFinder();
    void runDevice(int index, int id, DeviceType type, std::string address);
    int chooseId(const std::string &address) const;
    void printConnectedDevices();
    void shutDownDevices();

    AbstractHubListener *mListener;
    wiimote_t **mWiimotes = nullptr;     //!< one per slot, owned by a device thread while it's busy
    wiimote_t **mPlaceholders = nullptr; //!< stand-ins for busy slots during wiiuse_find(), see runFinder()
    std::map<std::string, int> mFixedIds; //!< by normalized address

    std::mutex mMutex; //!< guards mSlots
    Slot mSlots[sMaxDevices];
    std::thread mDeviceThreads[sMaxDevices];
    std::thread mFinderThread;
    std::atomic<bool> mIsRunning{false};
    bool mIsShuttingDownOnExit = false;
    std::chrono::milliseconds mIdleTimeout{0}; //!< 0: never
    std::shared_ptr<std::atomic<bool>> mIsPairing = std::make_shared<std::atomic<bool>>(false);
};
