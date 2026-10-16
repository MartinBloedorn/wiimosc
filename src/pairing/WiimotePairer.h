#pragma once

#include <string>
#include <vector>

/*! \brief Discovers Nintendo RVL devices (Wiimote, Balance Board) via Bluetooth inquiry and pairs them at OS level.

    Once paired, the devices show up as regular HID devices, and can be opened by wiiuse.
 */
class WiimotePairer
{
public:
    /*! \brief The passkey a device expects, which depends on how it was made discoverable.

        Using the wrong method makes the OS drop the connection immediately.
     */
    enum class Method {
        SyncButton, //!< Red SYNC button: passkey is the host address. Pairing is permanent, any button reconnects later.
        OneTwo,     //!< 1+2 buttons: passkey is the device address. Has to be redone after every power cycle.
        None        //!< No authentication, only enables the HID service (temporary connection, like 1+2).
    };

    struct Device {
        std::string name;
        std::string address;
    };

    WiimotePairer();
    ~WiimotePairer();

    WiimotePairer(const WiimotePairer &) = delete;
    WiimotePairer &operator=(const WiimotePairer &) = delete;

    /*! \brief Opens the first local Bluetooth radio. */
    bool open();

    std::string hostAddress() const;

    /*! \brief Removes Nintendo devices that the OS remembers but which are neither connected nor authenticated.

        Such leftovers are skipped by the inquiry and can't be re-enabled, so they block re-pairing.
     */
    int removeStaleDevices();

    /*! \brief Runs one inquiry and pairs every unconnected Nintendo device found.

        Blocks for roughly \a inquiryUnits * 1.28 seconds.
        \return the devices that were successfully paired and had their HID service enabled.
     */
    std::vector<Device> scanAndPair(Method method, int inquiryUnits = 3);

    /*! \brief Drops the Bluetooth connection to the device with \a address ("XX:XX:XX:XX:XX:XX").

        Nintendo devices power off when their connection drops, as when a Wii turns them off. The pairing is kept,
        so a SYNC-paired device reconnects on its next button press.
     */
    bool disconnect(const std::string &address);

private:
    void *mRadio = nullptr;
    unsigned char mHostAddress[6] = {};
};
