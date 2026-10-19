/*! \brief Pairing and test tool: pairs Wiimotes and Balance Boards, and reports connections and button presses. */

#include "hub/WiimoteHub.h"

#include <wiiuse.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

std::atomic<bool> sIsRunning{true};

/*! \brief Prints a line for every press of A (or the Balance Board's button, which is reported as A). */
class ButtonPrinter : public AbstractHubListener
{
public:
    void connected(int id, wiimote_t *, DeviceType type) override { mTypes[id] = type; }

    void event(int id, wiimote_t *wm) override
    {
        if (wm->event != WIIUSE_EVENT || !IS_JUST_PRESSED(wm, WIIMOTE_BUTTON_A))
            return;
        const DeviceType type = mTypes[id];
        std::printf("[%d] %s: %s pressed\n", id, deviceTypeName(type),
                    type == DeviceType::BalanceBoard ? "button" : "A");
    }

private:
    DeviceType mTypes[WiimoteHub::sMaxDevices] = {};
};

void printUsage()
{
    std::printf("usage: wiimosc-pair [--one-two | --no-auth] [--id <id>=<address> ...] [--shutdown-on-exit]\n"
                "                    [--idle-timeout <minutes>] [--scan-timeout <seconds>]\n"
                "  default              expects the red SYNC button (permanent pairing, works for the Balance Board)\n"
                "  --one-two            expects 1+2 to be held (temporary pairing)\n"
                "  --no-auth            skips authentication, only enables the HID service\n"
                "  --shutdown-on-exit   switches off all connected devices when quitting\n"
                "  --idle-timeout <min> switches off devices that have been idle this long (default 0 = never);\n"
                "                       counts buttons and Balance Board weight (motion isn't read by this tool)\n"
                "  --scan-timeout <s>   stops scanning for remotes to pair after this long (default 0 = never);\n"
                "                       remotes paired before still reconnect\n"
                "  --id <id>=<address>  always gives the device with this Bluetooth address this id (0-%d),\n"
                "                       e.g. --id 0=B8:AE:6E:56:1B:BD; repeatable\n",
                WiimoteHub::sMaxDevices - 1);
}

} // namespace

int main(int argc, char **argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    ButtonPrinter printer;
    WiimoteHub hub(&printer);
    WiimotePairer::Method method = WiimotePairer::Method::SyncButton;
    for (int i = 1; i < argc; ++i) {
        std::string error;
        if (std::strcmp(argv[i], "--one-two") == 0) {
            method = WiimotePairer::Method::OneTwo;
        } else if (std::strcmp(argv[i], "--idle-timeout") == 0 && i + 1 < argc) {
            const double minutes = std::atof(argv[++i]);
            if (minutes < 0.0) {
                printUsage();
                return 1;
            }
            hub.setIdleTimeout(minutes);
        } else if (std::strcmp(argv[i], "--scan-timeout") == 0 && i + 1 < argc) {
            const double seconds = std::atof(argv[++i]);
            if (seconds < 0.0) {
                printUsage();
                return 1;
            }
            hub.setScanTimeout(seconds);
        } else if (std::strcmp(argv[i], "--shutdown-on-exit") == 0) {
            hub.setShutdownOnExit(true);
        } else if (std::strcmp(argv[i], "--no-auth") == 0) {
            method = WiimotePairer::Method::None;
        } else if (std::strcmp(argv[i], "--id") == 0 && i + 1 < argc) {
            if (!hub.parseFixedId(argv[++i], &error)) {
                std::fprintf(stderr, "--id: %s\n", error.c_str());
                return 1;
            }
        } else {
            printUsage();
            return 1;
        }
    }

    std::signal(SIGINT, [](int) { sIsRunning = false; });
#ifdef SIGBREAK
    std::signal(SIGBREAK, [](int) { sIsRunning = false; }); // Ctrl+Break (Windows)
#endif

    if (!hub.start(method))
        return 1;
    std::printf("scanning, press %s on a remote to pair it, A to test it (Ctrl+C to quit)\n",
                method == WiimotePairer::Method::SyncButton ? "SYNC" : "1+2");

    while (sIsRunning)
        WiimotePairer::processEvents(100);
    hub.stop();
    return 0;
}
