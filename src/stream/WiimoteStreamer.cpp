#include "WiimoteStreamer.h"

#include "osc/OscReceiver.h"
#include "osc/OscSender.h"
#include "speaker/Audio.h"

#include <wiiuse.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>

// wiiuse-internal (io.h): reads registers, blocking until the reply arrives.
extern "C" void wiiuse_read_data_sync(struct wiimote_t *wm, byte memory, unsigned addr, unsigned short size,
                                      byte *data);

namespace {

struct ButtonName {
    int mask;
    const char *path;
};

const ButtonName sMoteButtons[] = {
    {WIIMOTE_BUTTON_A, "mote/button/a"},       {WIIMOTE_BUTTON_B, "mote/button/b"},
    {WIIMOTE_BUTTON_ONE, "mote/button/1"},     {WIIMOTE_BUTTON_TWO, "mote/button/2"},
    {WIIMOTE_BUTTON_PLUS, "mote/button/plus"}, {WIIMOTE_BUTTON_MINUS, "mote/button/minus"},
    {WIIMOTE_BUTTON_HOME, "mote/button/home"}, {WIIMOTE_BUTTON_UP, "mote/button/up"},
    {WIIMOTE_BUTTON_DOWN, "mote/button/down"}, {WIIMOTE_BUTTON_LEFT, "mote/button/left"},
    {WIIMOTE_BUTTON_RIGHT, "mote/button/right"},
};

const ButtonName sChuckButtons[] = {
    {NUNCHUK_BUTTON_C, "chuck/button/c"},
    {NUNCHUK_BUTTON_Z, "chuck/button/z"},
};

const char *sIrDotPaths[] = {"mote/ir/dot/0", "mote/ir/dot/1", "mote/ir/dot/2", "mote/ir/dot/3"};
const char *sBoardSensorPaths[] = {"fit/raw/0", "fit/raw/1", "fit/raw/2", "fit/raw/3"};
const char *sBoardCornerPaths[] = {"fit/corner/0", "fit/corner/1", "fit/corner/2", "fit/corner/3"};

const auto sBatteryInterval = std::chrono::seconds(10);
const auto sTareDuration = std::chrono::seconds(1);
/*! \brief Below this total weight (kg), the board's center of mass is reported as 0,0 instead of noise. */
const float sMinBoardWeight = 2.0f;
/*! \brief Above this intensity, mote/rumble/{trigger,pulse} switch the motor on. */
const float sRumbleThreshold = 0.5f;
const float sMaxRumblePulseMs = 3000.0f;
const char sRumblePath[] = "mote/rumble/";
/*! \brief Ignored addresses are printed once each, up to this many. */
const size_t sMaxIgnoredAddresses = 64;
/*! \brief The speaker's settings: wiimosc-speaker's defaults, which played best (see doc/Speaker.md). */
const WiimoteSpeaker::Settings sSpeakerSettings{};
const char sSpeakerPath[] = "mote/speaker/";

} // namespace

WiimoteStreamer::WiimoteStreamer(OscSender &sender, const Options &options)
    : mSender(sender)
    , mOptions(options)
{
    if (mOptions.isIrEnabled && mOptions.fusion != FilterType::None && mOptions.irAnchors.empty())
        mOptions.irAnchors.push_back(IrAnchor{});
}

void WiimoteStreamer::connected(int id, wiimote_t *wm, DeviceType type)
{
    setRumbleRequest(id, RumbleRequest{});
    Device &device = mDevices[id];
    device = Device{};
    device.type = type;
    device.nextBatteryRequest = Clock::now() + sBatteryInterval;

    const bool isBoard = type == DeviceType::BalanceBoard;
    send(OscMessage(address(id, "status")).add(std::string(isBoard ? "fit" : "mote")));
    if (!isBoard) {
        std::lock_guard<std::mutex> lock(mSpeakers[id].mutex);
        mSpeakers[id].wm = wm;
    }
    // The battery level from the connection handshake can be bogus, so ask for a fresh status report instead.
    wiiuse_status(wm);

    if (isBoard) {
        if (mOptions.isTaring) {
            device.isTaring = true;
            device.tareEnd = Clock::now() + sTareDuration;
            std::printf("[%d] taring, keep the board empty\n", id);
        }
        return;
    }

    wiiuse_motion_sensing(wm, 1);
    if (mOptions.isIrEnabled) {
        // Blocks this device's thread for ~100 ms (wiiuse waits for the camera's register writes).
        wiiuse_set_ir_sensitivity(wm, mOptions.irSensitivity);
        wiiuse_set_ir(wm, 1);
    }
    const bool hasMotionPlus = WIIMOTE_IS_SET(wm, WIIMOTE_STATE_MPLUS_PRESENT);
    if (mOptions.fusion != FilterType::None) {
        if (hasMotionPlus) {
            device.fusion = std::make_unique<MotionFusion>(mOptions.fusion);
            if (mOptions.isIrEnabled)
                device.fusion->setIrAnchors(mOptions.irAnchors);
        } else
            std::printf("[%d] no MotionPlus, no sensor fusion: wiiuse's mote/rpy\n", id);
    }
    if ((mOptions.isGyroEnabled || device.fusion) && hasMotionPlus) {
        // wiiuse's gyro (and the fusion's) calibrates at rest, so the remote should lie still.
        std::printf("[%d] enabling MotionPlus, keep the remote still\n", id);
        wiiuse_set_motion_plus(wm, 1);
    }
}

void WiimoteStreamer::disconnected(int id, DeviceType)
{
    send(OscMessage(address(id, "status")).add(std::string("none")));
    mDevices[id] = Device{};
    setRumbleRequest(id, RumbleRequest{});
    resetSpeaker(&mSpeakers[id]);
}

void WiimoteStreamer::stopSpeakers()
{
    for (Speaker &speaker : mSpeakers)
        resetSpeaker(&speaker);
}

void WiimoteStreamer::resetSpeaker(Speaker *speaker)
{
    std::lock_guard<std::mutex> lock(speaker->mutex);
    speaker->speaker.reset(); // mutes and switches off the speaker, if still connected
    speaker->player.reset();
    speaker->wm = nullptr;
}

void WiimoteStreamer::event(int id, wiimote_t *wm)
{
    if (wm->event == WIIUSE_STATUS)
        sendBattery(id, wm);
    else if (wm->event == WIIUSE_EVENT && mDevices[id].fusion)
        updateFusion(id, wm);
}

void WiimoteStreamer::updateFusion(int id, wiimote_t *wm)
{
    // Called per input report (update() also runs when the poll timed out). wiiuse only reports an event when
    // something changed, but the gyro's noise changes on nearly every report, and the time between reports is
    // measured anyway.
    Device &device = mDevices[id];
    if (wm->exp.type != EXP_MOTION_PLUS) {
        device.isFusionActive = false;
        return;
    }

    if (!device.isFusionActive) {
        // On activation (and again after the MotionPlus was deactivated): its calibration, then a fresh start.
        device.isFusionActive = true;
        byte data[MotionPlusCalibration::sSize];
        wiiuse_read_data_sync(wm, 0, MotionPlusCalibration::sAddress, sizeof(data), data);
        MotionPlusCalibration calibration;
        if (calibration.parse(data))
            std::printf("[%d] fusion: MotionPlus calibration read (range %.0f deg/s), keep the remote still\n", id,
                        calibration.range());
        else
            std::printf("[%d] fusion: MotionPlus calibration unreadable, using nominal values; keep the remote still\n",
                        id);
        device.fusion->setCalibration(calibration);
        return;
    }

    const motion_plus_t &mp = wm->exp.mp;
    const uint16_t rawGyro[3] = {static_cast<uint16_t>(mp.raw_gyro.pitch), static_cast<uint16_t>(mp.raw_gyro.roll),
                                 static_cast<uint16_t>(mp.raw_gyro.yaw)};
    // acc_mode bits: roll (4), pitch (2), yaw (1); set while slow.
    const bool isSlow[3] = {(mp.acc_mode & 0x02) != 0, (mp.acc_mode & 0x04) != 0, (mp.acc_mode & 0x01) != 0};
    const double time = std::chrono::duration<double>(Clock::now().time_since_epoch()).count();

    IrFrame ir;
    for (const ir_dot_t &dot : wm->ir.dot) {
        if (dot.visible)
            ir.dots[ir.count++] = IrFrame::Dot{static_cast<float>(dot.rx), static_cast<float>(dot.ry)};
    }

    const bool wasReady = device.fusion->isReady();
    device.fusion->addReport(time, rawGyro, isSlow, Vector3{wm->gforce.x, wm->gforce.y, wm->gforce.z},
                             mOptions.isIrEnabled ? &ir : nullptr);
    if (!wasReady && device.fusion->isReady()) {
        const FilterSetup &setup = device.fusion->setup();
        std::printf("[%d] fusion: ready (gyro bias %.2f, %.2f, %.2f deg/s; %.0f reports/s)%s\n", id,
                    setup.gyroBias.x, setup.gyroBias.y, setup.gyroBias.z, setup.sampleRate,
                    device.fusion->irCorrection() ? "; point at the home anchor to sync the yaw" : "");
    }
    const IrAnchorCorrection *correction = device.fusion->irCorrection();
    const bool isSynced = correction && correction->isSynced();
    if (isSynced && !device.isIrSynced)
        std::printf("[%d] fusion: yaw synced to the home anchor\n", id);
    device.isIrSynced = isSynced;
}

void WiimoteStreamer::update(int id, wiimote_t *wm)
{
    Device &device = mDevices[id];
    const auto now = Clock::now();

    if (now >= device.nextBatteryRequest) {
        wiiuse_status(wm); // answered with a WIIUSE_STATUS event
        device.nextBatteryRequest = now + sBatteryInterval;
    }

    const bool isSendingContinuous = now >= device.nextContinuous;
    if (isSendingContinuous && mOptions.maxRate > 0)
        device.nextContinuous = now + std::chrono::microseconds(1000000 / mOptions.maxRate);

    if (device.type == DeviceType::BalanceBoard) {
        updateBoard(id, wm, isSendingContinuous);
    } else {
        applyRumble(id, wm);
        updateWiimote(id, wm, isSendingContinuous);
    }
}

void WiimoteStreamer::applyRumble(int id, wiimote_t *wm)
{
    // Nothing until requested, so the hub's connection rumble runs undisturbed. Compared with the motor's actual
    // state rather than the last request, so a request made during that rumble outlasts its end.
    int state;
    {
        std::lock_guard<std::mutex> lock(mRumbleMutex);
        RumbleRequest &request = mRumbleRequests[id];
        if (request.isPulse && Clock::now() >= request.pulseEnd) {
            request.state = 0;
            request.isPulse = false;
        }
        state = request.state;
    }
    const bool isRumbling = WIIMOTE_IS_SET(wm, WIIMOTE_STATE_RUMBLE);
    if (state >= 0 && (state == 1) != isRumbling)
        wiiuse_rumble(wm, state);
}

void WiimoteStreamer::receivedRumble(int id, const std::string &command, const OscReceivedMessage &message)
{
    float intensity;
    RumbleRequest request;
    if (command == "trigger") {
        if (!message.toFloat(0, &intensity))
            return ignore(message, "expects an intensity (0-1)");
        request.state = intensity > sRumbleThreshold ? 1 : 0;
    } else if (command == "pulse") {
        float durationMs;
        if (!message.toFloat(0, &intensity) || !message.toFloat(1, &durationMs))
            return ignore(message, "expects an intensity (0-1) and a duration (ms)");
        if (durationMs > sMaxRumblePulseMs)
            durationMs = sMaxRumblePulseMs;
        // A pulse that wouldn't rumble still overrides the ongoing request: it switches the motor off.
        request.state = intensity > sRumbleThreshold && durationMs > 0.0f ? 1 : 0;
        request.isPulse = request.state == 1;
        request.pulseEnd = Clock::now() + std::chrono::microseconds(static_cast<long long>(durationMs * 1000.0f));
    } else {
        return ignore(message, "unknown address");
    }
    setRumbleRequest(id, request);
}

void WiimoteStreamer::setRumbleRequest(int id, const RumbleRequest &request)
{
    std::lock_guard<std::mutex> lock(mRumbleMutex);
    mRumbleRequests[id] = request;
}

void WiimoteStreamer::received(const OscReceivedMessage &message)
{
    // <prefix>/<id>/<path>
    const std::string &prefix = mOptions.prefix;
    const std::string &addr = message.address;
    size_t position = prefix.size();
    if (addr.compare(0, position, prefix) != 0 || position >= addr.size() || addr[position] != '/')
        return ignore(message, "unknown address");
    int id = 0;
    const size_t idStart = ++position;
    while (position < addr.size() && position - idStart < 3 && std::isdigit(static_cast<unsigned char>(addr[position])))
        id = id * 10 + (addr[position++] - '0');
    if (position == idStart || position >= addr.size() || addr[position] != '/')
        return ignore(message, "unknown address");
    const std::string path = addr.substr(position + 1);

    const bool isSpeaker = path.compare(0, sizeof(sSpeakerPath) - 1, sSpeakerPath) == 0;
    const bool isRumble = path.compare(0, sizeof(sRumblePath) - 1, sRumblePath) == 0;
    if (!isRumble && !isSpeaker)
        return ignore(message, "unknown address");
    if (id >= WiimoteHub::sMaxDevices)
        return ignore(message, "id out of range");
    if (isSpeaker)
        return receivedSpeaker(id, path.substr(sizeof(sSpeakerPath) - 1), message);
    receivedRumble(id, path.substr(sizeof(sRumblePath) - 1), message);
}

void WiimoteStreamer::receivedSpeaker(int id, const std::string &command, const OscReceivedMessage &message)
{
    Speaker &speaker = mSpeakers[id];
    const char *notConnected = "no Wiimote connected with this id";
    const float maxFrequency = sSpeakerSettings.sampleRate / 2.0f;
    float frequency = 0.0f;
    std::shared_ptr<std::vector<float>> clip;
    if (command == "sine") {
        if (!message.toFloat(0, &frequency) || !(frequency > 0.0f && frequency <= maxFrequency))
            return ignore(message, "expects a frequency in Hz (up to 3000)");
    } else if (command == "file") {
        if (message.arguments.empty() || (message.arguments[0].type != 's' && message.arguments[0].type != 'S'))
            return ignore(message, "expects a file path (string)");
        {
            std::lock_guard<std::mutex> lock(speaker.mutex);
            if (!speaker.wm)
                return ignore(message, notConnected);
        }
        // Decoded here, without holding the lock: other OSC input waits meanwhile (~2 s for a 4-minute song).
        const std::string &path = message.arguments[0].string;
        const auto loadStart = Clock::now();
        clip = std::make_shared<std::vector<float>>();
        std::string error;
        if (!loadAudioFile(path, sSpeakerSettings.sampleRate, clip.get(), &error)) {
            std::printf("[%d] speaker: can't play %s: %s\n", id, path.c_str(), error.c_str());
            return;
        }
        normalize(clip.get(), sSoundPeak);
        std::printf("[%d] speaker: playing %s (%.1f s, decoded in %.0f ms)\n", id, path.c_str(),
                    static_cast<double>(clip->size()) / sSpeakerSettings.sampleRate,
                    std::chrono::duration<double, std::milli>(Clock::now() - loadStart).count());
    } else if (command != "stop") {
        return ignore(message, "unknown address");
    }

    std::lock_guard<std::mutex> lock(speaker.mutex);
    if (!speaker.wm)
        return ignore(message, notConnected);
    if (command == "stop") {
        if (speaker.player)
            speaker.player->stop();
        return;
    }
    if (!speaker.speaker) {
        speaker.player = std::make_unique<SoundPlayer>(sSpeakerSettings.sampleRate);
        speaker.speaker = std::make_unique<WiimoteSpeaker>(speaker.wm, id, sSpeakerSettings, speaker.player.get());
        speaker.speaker->start();
    }
    if (clip)
        speaker.player->playClip(std::move(clip));
    else
        speaker.player->playSine(frequency);
}

void WiimoteStreamer::ignore(const OscReceivedMessage &message, const char *reason)
{
    if (mIgnoredAddresses.size() < sMaxIgnoredAddresses && mIgnoredAddresses.insert(message.address).second)
        std::printf("[osc] ignoring %s: %s (printed once per address)\n", message.address.c_str(), reason);
}

void WiimoteStreamer::updateWiimote(int id, wiimote_t *wm, bool isSendingContinuous)
{
    for (const ButtonName &button : sMoteButtons)
        sendInt(id, button.path, (wm->btns & button.mask) ? 1 : 0);

    // With the MotionPlus active, a Nunchuk on its pass-through port isn't read.
    const bool hasNunchuk = wm->exp.type == EXP_NUNCHUK;
    sendInt(id, "chuck/status", hasNunchuk ? 1 : 0);
    const nunchuk_t &nunchuk = wm->exp.nunchuk;
    if (hasNunchuk) {
        for (const ButtonName &button : sChuckButtons)
            sendInt(id, button.path, (nunchuk.btns & button.mask) ? 1 : 0);
    }
    const ir_t &ir = wm->ir;
    if (mOptions.isIrEnabled)
        sendInt(id, "mote/ir/count", ir.num_dots); // immediately, like buttons: it says which positions are valid
    const MotionFusion *fusion = mDevices[id].fusion.get();
    if (fusion && fusion->isReady() && fusion->irCorrection())
        sendInt(id, "mote/ir/anchor", fusion->irCorrection()->anchorInView());

    if (!isSendingContinuous)
        return;

    // Positions are only sent while valid: without any visible source, wiiuse reports 0,0.
    if (mOptions.isIrEnabled && ir.num_dots > 0) {
        // The pointer, as wiiuse computes it from the visible sources (roll-corrected, estimated from one source
        // when the other of a sensor bar's pair is lost), on its virtual screen (ir.vres, 560x420 by default).
        // Both it and the dots are mapped to -1..1 with y up, like the other xy values. z is wiiuse's distance
        // measure, 1023 minus the pixels between the first two sources, mapped to 0..1 (larger: farther); 0 until
        // two sources were seen.
        sendFloats(id, "mote/ir/xyz",
                   {2.0f * ir.x / ir.vres[0] - 1.0f, 1.0f - 2.0f * ir.y / ir.vres[1], ir.z / 1023.0f});
        for (int i = 0; i < 4; ++i) {
            // Raw camera positions (1024x768), not roll-corrected: for any arrangement of IR sources.
            const ir_dot_t &dot = ir.dot[i];
            if (dot.visible)
                sendFloats(id, sIrDotPaths[i], {2.0f * dot.rx / 1023.0f - 1.0f, 1.0f - 2.0f * dot.ry / 767.0f});
        }
    }

    if (!fusion) {
        sendFloats(id, "mote/rpy", {wm->orient.roll, wm->orient.pitch, wm->orient.yaw});
    } else if (fusion->isReady()) {
        // Nothing while calibrating: receivers shouldn't mix wiiuse's angles with fused ones.
        const Quaternion q = fusion->orientation();
        const EulerAngles angles = toEulerAngles(q);
        sendFloats(id, "mote/rpy", {angles.roll, angles.pitch, angles.yaw});
        sendFloats(id, "mote/quat", {q.w, q.x, q.y, q.z});
    }
    sendFloats(id, "mote/accel", {wm->gforce.x, wm->gforce.y, wm->gforce.z});
    if (wm->exp.type == EXP_MOTION_PLUS) {
        const ang3f_t &rate = wm->exp.mp.angle_rate_gyro;
        sendFloats(id, "mote/gyro", {rate.roll, rate.pitch, rate.yaw});
    }
    if (hasNunchuk) {
        sendFloats(id, "chuck/xy", {nunchuk.js.x, nunchuk.js.y});
        sendFloats(id, "chuck/rpy", {nunchuk.orient.roll, nunchuk.orient.pitch, nunchuk.orient.yaw});
        sendFloats(id, "chuck/accel", {nunchuk.gforce.x, nunchuk.gforce.y, nunchuk.gforce.z});
    }
}

void WiimoteStreamer::updateBoard(int id, wiimote_t *wm, bool isSendingContinuous)
{
    // The board's front button is reported as the Wiimote's A button.
    sendInt(id, "fit/button", (wm->btns & WIIMOTE_BUTTON_A) ? 1 : 0);

    Device &device = mDevices[id];
    const wii_board_t &board = wm->exp.wb;
    const float sensors[4] = {board.tl, board.tr, board.bl, board.br};

    if (device.isTaring) {
        for (int i = 0; i < 4; ++i)
            device.tareSums[i] += sensors[i];
        ++device.tareSamples;
        if (Clock::now() < device.tareEnd)
            return;
        float total = 0.0f;
        for (int i = 0; i < 4; ++i) {
            device.tareOffsets[i] = static_cast<float>(device.tareSums[i] / device.tareSamples);
            total += device.tareOffsets[i];
        }
        device.isTaring = false;
        std::printf("[%d] tared, offset %.2f kg\n", id, total);
    }

    if (!isSendingContinuous)
        return;

    float corrected[4];
    float weight = 0.0f;
    for (int i = 0; i < 4; ++i) {
        corrected[i] = sensors[i] - device.tareOffsets[i];
        weight += corrected[i];
        sendFloats(id, sBoardSensorPaths[i], {corrected[i]});
    }
    sendFloats(id, "fit/weight", {weight});

    // Each corner's share of the total weight; all 0 for an (almost) empty board, which also avoids dividing by 0.
    const bool isLoaded = weight >= sMinBoardWeight;
    for (int i = 0; i < 4; ++i)
        sendFloats(id, sBoardCornerPaths[i], {isLoaded ? corrected[i] / weight : 0.0f});

    // Sensor order: top left, top right, bottom left, bottom right.
    float x = 0.0f;
    float y = 0.0f;
    if (isLoaded) {
        x = ((corrected[1] + corrected[3]) - (corrected[0] + corrected[2])) / weight;
        y = ((corrected[0] + corrected[1]) - (corrected[2] + corrected[3])) / weight;
    }
    sendFloats(id, "fit/xy", {x, y});
}

std::string WiimoteStreamer::address(int id, const char *path) const
{
    return mOptions.prefix + "/" + std::to_string(id) + "/" + path;
}

void WiimoteStreamer::send(const OscMessage &message)
{
    mSender.send(message);
}

void WiimoteStreamer::sendFloats(int id, const char *path, std::initializer_list<float> values)
{
    std::vector<float> &sent = mDevices[id].sentValues[path];
    if (sent.size() == values.size() && std::equal(values.begin(), values.end(), sent.begin()))
        return;
    sent.assign(values);

    OscMessage message(address(id, path));
    for (float value : values)
        message.add(value);
    send(message);
}

void WiimoteStreamer::sendInt(int id, const char *path, int value)
{
    std::vector<float> &sent = mDevices[id].sentValues[path];
    if (sent.size() == 1 && sent[0] == static_cast<float>(value))
        return;
    sent.assign({static_cast<float>(value)});
    send(OscMessage(address(id, path)).add(static_cast<int32_t>(value)));
}

void WiimoteStreamer::sendBattery(int id, const wiimote_t *wm)
{
    send(OscMessage(address(id, "battery")).add(wm->battery_level));
}
