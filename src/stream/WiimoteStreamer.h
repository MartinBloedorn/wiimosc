#pragma once

#include "hub/WiimoteHub.h"

#include <chrono>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>

class OscMessage;
class OscSender;

/*! \brief Streams the input of all connected devices as OSC messages.

    Addresses have the form <prefix>/<id>/<path>, see doc/Streaming.md. Values are only sent when they change. Buttons
    are sent immediately, continuous values (orientation, acceleration, ...) at most \c maxRate times per second.
 */
class WiimoteStreamer : public AbstractHubListener
{
public:
    struct Options {
        std::string prefix;       //!< normalized: empty, or starting with '/' and without trailing '/'
        int maxRate = 0;          //!< per device, in Hz; 0 is unlimited
        bool isGyroEnabled = false; //!< activates the MotionPlus on supporting remotes (hides a Nunchuk)
        bool isTaring = false;    //!< zeroes the Balance Board's sensors after connecting
    };

    WiimoteStreamer(OscSender &sender, const Options &options);

    void connected(int id, wiimote_t *wm, DeviceType type) override;
    void disconnected(int id, DeviceType type) override;
    void event(int id, wiimote_t *wm) override;
    void update(int id, wiimote_t *wm) override;

private:
    using Clock = std::chrono::steady_clock;

    struct Device {
        DeviceType type = DeviceType::Wiimote;
        Clock::time_point nextContinuous;
        Clock::time_point nextBatteryRequest;
        std::map<std::string, std::vector<float>> sentValues; //!< by path, for change detection

        bool isTaring = false;
        Clock::time_point tareEnd;
        double tareSums[4] = {};
        int tareSamples = 0;
        float tareOffsets[4] = {};
    };

    std::string address(int id, const char *path) const;
    void send(const OscMessage &message);

    /*! \brief Sends the values as floats, if they differ from the ones last sent on \a path. */
    void sendFloats(int id, const char *path, std::initializer_list<float> values);
    /*! \brief Sends the value as int, if it differs from the one last sent on \a path. */
    void sendInt(int id, const char *path, int value);
    void sendBattery(int id, const wiimote_t *wm);

    void updateWiimote(int id, wiimote_t *wm, bool isSendingContinuous);
    void updateBoard(int id, wiimote_t *wm, bool isSendingContinuous);

    OscSender &mSender;
    Options mOptions;
    Device mDevices[WiimoteHub::sMaxDevices];
};
