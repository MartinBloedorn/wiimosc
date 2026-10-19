#pragma once

#include "fusion/MotionFusion.h"
#include "hub/WiimoteHub.h"
#include "speaker/SoundSources.h"
#include "speaker/WiimoteSpeaker.h"

#include <atomic>
#include <chrono>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

class OscMessage;
class OscSender;
struct OscReceivedMessage;

/*! \brief Streams the input of all connected devices as OSC messages.

    Addresses have the form <prefix>/<id>/<path>, see doc/Streaming.md. Values are only sent when they change. Buttons
    are sent immediately, continuous values (orientation, acceleration, ...) at most \c maxRate times per second.

    It also takes OSC input (received()): <prefix>/<id>/mote/rumble switches a remote's rumble motor, and
    <prefix>/<id>/mote/speaker/{sine,file,stop} play sounds on its speaker.
 */
class WiimoteStreamer : public AbstractHubListener
{
public:
    struct Options {
        std::string prefix;       //!< normalized: empty, or starting with '/' and without trailing '/'
        int maxRate = 0;          //!< per device, in Hz; 0 is unlimited
        bool isGyroEnabled = false; //!< activates the MotionPlus on supporting remotes (hides a Nunchuk)
        /*! \brief Sensor fusion on remotes with a MotionPlus (which it activates): mote/rpy and mote/quat. */
        FilterType fusion = FilterType::None;
        bool isTaring = false;    //!< zeroes the Balance Board's sensors after connecting
        bool isIrEnabled = false; //!< activates the IR camera, and streams the IR sources it sees
        int irSensitivity = 3;    //!< IR camera sensitivity, 1 (least) to 5 (most); wiiuse's default is 3
        /*! \brief With fusion: IR anchors that correct the yaw, the home anchor (a sensor bar) first.

            With --ir and fusion, but no anchors given, it's just the home anchor, at yaw 0.
         */
        std::vector<IrAnchor> irAnchors;
    };

    WiimoteStreamer(OscSender &sender, const Options &options);

    void connected(int id, wiimote_t *wm, DeviceType type) override;
    void disconnected(int id, DeviceType type) override;
    void event(int id, wiimote_t *wm) override;
    void update(int id, wiimote_t *wm) override;

    /*! \brief Handles an OSC message received on the input port. Called from the receiver's thread.

        Requests are stored, and applied by the device's own thread on its next update(); requests for ids that
        aren't connected are dropped.
     */
    void received(const OscReceivedMessage &message);

    /*! \brief Stops all speakers; the hub doesn't call disconnected() for devices still connected on shutdown. */
    void stopSpeakers();

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

        std::unique_ptr<MotionFusion> fusion; //!< only with Options::fusion, on remotes with a MotionPlus
        bool isFusionActive = false;          //!< the MotionPlus reports, and its calibration was read
        bool isIrSynced = false;              //!< the fusion's yaw refers to the IR anchors
    };

    /*! \brief A remote's speaker. Set up on the first mote/speaker request, and kept until the remote disconnects,
        so later sounds start without the setup's delay (~0.3 s).

        Used by the receiver's thread (requests) and the device's thread (connect, disconnect), hence the mutex.
     */
    struct Speaker {
        std::mutex mutex;
        wiimote_t *wm = nullptr; //!< while a Wiimote is connected
        std::unique_ptr<SoundPlayer> player;
        std::unique_ptr<WiimoteSpeaker> speaker; //!< plays player; destroyed first
    };

    std::string address(int id, const char *path) const;
    void send(const OscMessage &message);

    /*! \brief Sends the values as floats, if they differ from the ones last sent on \a path. */
    void sendFloats(int id, const char *path, std::initializer_list<float> values);
    /*! \brief Sends the value as int, if it differs from the one last sent on \a path. */
    void sendInt(int id, const char *path, int value);
    void sendBattery(int id, const wiimote_t *wm);

    /*! \brief Feeds an input report to the device's MotionFusion. */
    void updateFusion(int id, wiimote_t *wm);
    void updateWiimote(int id, wiimote_t *wm, bool isSendingContinuous);
    void updateBoard(int id, wiimote_t *wm, bool isSendingContinuous);
    void applyRumble(int id, wiimote_t *wm);
    /*! \brief Handles <prefix>/<id>/mote/speaker/<command>. Called from the receiver's thread. */
    void receivedSpeaker(int id, const std::string &command, const OscReceivedMessage &message);
    static void resetSpeaker(Speaker *speaker);
    /*! \brief Prints why \a message is ignored, once per address. Called from the receiver's thread. */
    void ignore(const OscReceivedMessage &message, const char *reason);

    OscSender &mSender;
    Options mOptions;
    Device mDevices[WiimoteHub::sMaxDevices];

    /*! \brief Rumble per id, as last requested over OSC: 1 on, 0 off, -1 not requested since connecting.

        Written by the receiver's thread, applied by the device's thread. Kept apart from Device, which is reset by
        assignment.
     */
    std::atomic<int> mRumbleRequests[WiimoteHub::sMaxDevices];
    Speaker mSpeakers[WiimoteHub::sMaxDevices]; //!< kept apart from Device for the same reason
    std::set<std::string> mIgnoredAddresses; //!< receiver's thread only
};
