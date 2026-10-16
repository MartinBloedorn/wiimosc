#include "WiimoteStreamer.h"

#include "osc/OscSender.h"

#include <wiiuse.h>

#include <algorithm>
#include <cstdio>
#include <string>

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

const char *sBoardSensorPaths[] = {"fit/raw/0", "fit/raw/1", "fit/raw/2", "fit/raw/3"};
const char *sBoardCornerPaths[] = {"fit/corner/0", "fit/corner/1", "fit/corner/2", "fit/corner/3"};

const auto sBatteryInterval = std::chrono::seconds(10);
const auto sTareDuration = std::chrono::seconds(1);
/*! \brief Below this total weight (kg), the board's center of mass is reported as 0,0 instead of noise. */
const float sMinBoardWeight = 2.0f;

} // namespace

WiimoteStreamer::WiimoteStreamer(OscSender &sender, const Options &options)
    : mSender(sender)
    , mOptions(options)
{
}

void WiimoteStreamer::connected(int id, wiimote_t *wm, DeviceType type)
{
    Device &device = mDevices[id];
    device = Device{};
    device.type = type;
    device.nextBatteryRequest = Clock::now() + sBatteryInterval;

    const bool isBoard = type == DeviceType::BalanceBoard;
    send(OscMessage(address(id, "status")).add(std::string(isBoard ? "fit" : "mote")));
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
    if (mOptions.isGyroEnabled && WIIMOTE_IS_SET(wm, WIIMOTE_STATE_MPLUS_PRESENT)) {
        // The gyro calibrates on its first reading, so the remote should lie still.
        std::printf("[%d] enabling MotionPlus, keep the remote still\n", id);
        wiiuse_set_motion_plus(wm, 1);
    }
}

void WiimoteStreamer::disconnected(int id, DeviceType)
{
    send(OscMessage(address(id, "status")).add(std::string("none")));
    mDevices[id] = Device{};
}

void WiimoteStreamer::event(int id, wiimote_t *wm)
{
    if (wm->event == WIIUSE_STATUS)
        sendBattery(id, wm);
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

    if (device.type == DeviceType::BalanceBoard)
        updateBoard(id, wm, isSendingContinuous);
    else
        updateWiimote(id, wm, isSendingContinuous);
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

    if (!isSendingContinuous)
        return;

    sendFloats(id, "mote/rpy", {wm->orient.roll, wm->orient.pitch, wm->orient.yaw});
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
