/*! \brief Streaming tool: pairs devices like wiimosc-pair, and streams their input as OSC. */

#include "WiimoteStreamer.h"
#include "osc/OscSender.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> sIsRunning{true};

/*! \brief Turns "wiimosc", "/wiimosc/" etc. into "/wiimosc"; an empty prefix stays empty. */
std::string normalizePrefix(std::string prefix)
{
    while (!prefix.empty() && prefix.back() == '/')
        prefix.pop_back();
    if (!prefix.empty() && prefix.front() != '/')
        prefix.insert(prefix.begin(), '/');
    return prefix;
}

void printUsage()
{
    std::printf("usage: wiimosc-stream [options]\n"
                "  --host <host>      OSC target, name, IPv4 or broadcast address (default 127.0.0.1)\n"
                "  --port <port>      OSC target port (default 4321)\n"
                "  --prefix <prefix>  address prefix (default \"wiimosc\")\n"
                "  --max-rate <hz>    max. rate of continuous values per device, 0 = unlimited (default 0)\n"
                "  --gyro             enable the MotionPlus gyro on supporting remotes (hides a Nunchuk)\n"
                "  --fusion <madg|off>\n"
                "                     sensor fusion on remotes with a MotionPlus (hides a Nunchuk): fused mote/rpy\n"
                "                     including yaw, and mote/quat; madg = Madgwick's AHRS (default off)\n"
                "  --tare             zero the Balance Board after connecting (keep it empty)\n"
                "  --ir [anchor=<yaw[:elevation],...>]\n"
                "                     enable the IR camera, and stream the IR sources it sees. With --fusion, IR\n"
                "                     anchors correct the yaw: the first is a sensor bar (home), the others single\n"
                "                     sources, e.g. anchor=0,-60,60:20 (degrees as in mote/rpy; default: home at 0)\n"
                "  --ir-sensitivity <1-5>\n"
                "                     IR camera sensitivity (default 3)\n"
                "  --one-two          pair remotes held at 1+2 instead of SYNC\n"
                "  --no-auth          skip authentication when pairing\n"
                "  --shutdown-on-exit switch off all connected devices when quitting\n"
                "  --idle-timeout <minutes>\n"
                "                     switch off devices that have been idle this long (default 0 = never)\n"
                "  --id <id>=<addr>   always give the device with this Bluetooth address this id (0-%d),\n"
                "                     e.g. --id 0=B8:AE:6E:56:1B:BD; repeatable\n",
                WiimoteHub::sMaxDevices - 1);
}

} // namespace

int main(int argc, char **argv)
{
    std::string host = "127.0.0.1";
    int port = 4321;
    WiimoteStreamer::Options options;
    options.prefix = "wiimosc";
    WiimotePairer::Method method = WiimotePairer::Method::SyncButton;
    std::vector<std::string> fixedIds;
    bool isShuttingDownOnExit = false;
    double idleTimeout = 0.0;

    for (int i = 1; i < argc; ++i) {
        const bool hasValue = i + 1 < argc;
        if (std::strcmp(argv[i], "--host") == 0 && hasValue) {
            host = argv[++i];
        } else if (std::strcmp(argv[i], "--port") == 0 && hasValue) {
            port = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--prefix") == 0 && hasValue) {
            options.prefix = argv[++i];
        } else if (std::strcmp(argv[i], "--max-rate") == 0 && hasValue) {
            options.maxRate = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--gyro") == 0) {
            options.isGyroEnabled = true;
        } else if (std::strcmp(argv[i], "--fusion") == 0 && hasValue) {
            if (!parseFilterType(argv[++i], &options.fusion)) {
                printUsage();
                return 1;
            }
        } else if (std::strcmp(argv[i], "--tare") == 0) {
            options.isTaring = true;
        } else if (std::strcmp(argv[i], "--ir") == 0) {
            options.isIrEnabled = true;
            if (hasValue && std::strncmp(argv[i + 1], "anchor=", 7) == 0
                && !parseIrAnchors(argv[++i] + 7, &options.irAnchors)) {
                printUsage();
                return 1;
            }
        } else if (std::strcmp(argv[i], "--ir-sensitivity") == 0 && hasValue) {
            options.irSensitivity = std::atoi(argv[++i]);
            if (options.irSensitivity < 1 || options.irSensitivity > 5) {
                printUsage();
                return 1;
            }
        } else if (std::strcmp(argv[i], "--one-two") == 0) {
            method = WiimotePairer::Method::OneTwo;
        } else if (std::strcmp(argv[i], "--idle-timeout") == 0 && hasValue) {
            idleTimeout = std::atof(argv[++i]);
        } else if (std::strcmp(argv[i], "--shutdown-on-exit") == 0) {
            isShuttingDownOnExit = true;
        } else if (std::strcmp(argv[i], "--no-auth") == 0) {
            method = WiimotePairer::Method::None;
        } else if (std::strcmp(argv[i], "--id") == 0 && hasValue) {
            fixedIds.push_back(argv[++i]);
        } else {
            printUsage();
            return 1;
        }
    }
    if (port <= 0 || port > 65535 || options.maxRate < 0 || idleTimeout < 0.0) {
        printUsage();
        return 1;
    }
    if (!options.irAnchors.empty() && options.fusion == FilterType::None) {
        std::fprintf(stderr, "--ir anchor=... needs --fusion: the anchors correct the fused yaw\n");
        return 1;
    }
    if (host == "0.0.0.0") {
        std::fprintf(stderr, "0.0.0.0 is a listening address, not a target; use 127.0.0.1 or a broadcast address\n");
        return 1;
    }
    options.prefix = normalizePrefix(options.prefix);

    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::signal(SIGINT, [](int) { sIsRunning = false; });
#ifdef SIGBREAK
    std::signal(SIGBREAK, [](int) { sIsRunning = false; }); // Ctrl+Break (Windows)
#endif

    OscSender sender;
    if (!sender.open(host, static_cast<uint16_t>(port))) {
        std::fprintf(stderr, "can't resolve OSC target %s\n", host.c_str());
        return 1;
    }

    WiimoteStreamer streamer(sender, options);
    WiimoteHub hub(&streamer);
    for (const std::string &fixedId : fixedIds) {
        std::string error;
        if (!hub.parseFixedId(fixedId, &error)) {
            std::fprintf(stderr, "--id: %s\n", error.c_str());
            return 1;
        }
    }
    hub.setShutdownOnExit(isShuttingDownOnExit);
    hub.setIdleTimeout(idleTimeout);
    if (!hub.start(method))
        return 1;
    std::printf("streaming to %s:%d as %s/<id>/..., press %s on a remote to pair it (Ctrl+C to quit)\n",
                host.c_str(), port, options.prefix.c_str(),
                method == WiimotePairer::Method::SyncButton ? "SYNC" : "1+2");

    while (sIsRunning)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    hub.stop();
    return 0;
}
