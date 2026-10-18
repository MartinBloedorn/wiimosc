/*! \brief Recording tool: logs the raw motion data of the first Wiimote Plus to a CSV, for replaying it offline.

    Format (see data/recordings/README.md): comment lines start with '#': the column header, "#cal" with the
    MotionPlus calibration's 32 bytes, and "#phase <n> <time>" for each cued step. Data rows: time (s, steady clock),
    raw pitch, roll and yaw gyro values, their slow-mode flags, and the acceleration in g (x, y, z).
 */

#include "fusion/MotionPlusCalibration.h"
#include "hub/WiimoteHub.h"

#include <wiiuse.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// wiiuse-internal (io.h): reads registers, blocking until the reply arrives.
extern "C" void wiiuse_read_data_sync(struct wiimote_t *wm, byte memory, unsigned addr, unsigned short size,
                                      byte *data);

namespace {

std::atomic<bool> sIsRunning{true};

double now()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

/*! \brief Activates the MotionPlus of the first Wiimote Plus, and writes its reports to a file. */
class Recorder : public AbstractHubListener
{
public:
    explicit Recorder(FILE *file)
        : mFile(file)
    {
        std::fprintf(mFile, "# t,rawPitch,rawRoll,rawYaw,slowPitch,slowRoll,slowYaw,ax,ay,az\n");
    }

    /*! \brief Records only the device with this id (-1: the first Wiimote Plus). */
    void setRecordedId(int id) { mRecordedId = id; }

    void connected(int id, wiimote_t *wm, DeviceType type) override
    {
        if (type != DeviceType::WiimotePlus || mId >= 0 || (mRecordedId >= 0 && id != mRecordedId))
            return;
        mId = id;
        std::printf("[%d] recording, enabling MotionPlus\n", id);
        wiiuse_motion_sensing(wm, 1);
        wiiuse_set_motion_plus(wm, 1);
    }

    void disconnected(int id, DeviceType) override
    {
        if (id == mId) {
            std::printf("[%d] disconnected, recording stopped\n", id);
            sIsRunning = false;
        }
    }

    void event(int id, wiimote_t *wm) override
    {
        if (id != mId || wm->event != WIIUSE_EVENT || wm->exp.type != EXP_MOTION_PLUS)
            return;
        if (!mRemote) {
            // Like the streamer: the calibration once the MotionPlus reports, then the data.
            byte data[MotionPlusCalibration::sSize];
            wiiuse_read_data_sync(wm, 0, MotionPlusCalibration::sAddress, sizeof(data), data);
            std::lock_guard<std::mutex> lock(mMutex);
            std::fprintf(mFile, "#cal");
            for (byte b : data)
                std::fprintf(mFile, " %02x", b);
            std::fprintf(mFile, "\n");
            mRemote = wm;
            return;
        }
        const motion_plus_t &mp = wm->exp.mp;
        std::lock_guard<std::mutex> lock(mMutex);
        std::fprintf(mFile, "%.6f,%u,%u,%u,%d,%d,%d,%.5f,%.5f,%.5f\n", now(), static_cast<uint16_t>(mp.raw_gyro.pitch),
                     static_cast<uint16_t>(mp.raw_gyro.roll), static_cast<uint16_t>(mp.raw_gyro.yaw),
                     (mp.acc_mode & 0x02) != 0, (mp.acc_mode & 0x04) != 0, (mp.acc_mode & 0x01) != 0, wm->gforce.x,
                     wm->gforce.y, wm->gforce.z);
        ++mCount;
    }

    /*! \brief The remote once its MotionPlus reports, else nullptr. */
    wiimote_t *remote() const { return mRemote; }
    int count() const { return mCount; }

    void markPhase(int phase)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        std::fprintf(mFile, "#phase %d %.6f\n", phase, now());
    }

private:
    FILE *mFile;
    std::mutex mMutex;
    int mRecordedId = -1;
    int mId = -1;
    std::atomic<wiimote_t *> mRemote{nullptr};
    std::atomic<int> mCount{0};
};

void buzz(wiimote_t *wm, int milliseconds)
{
    // Rumble disturbs both sensors for ~0.3 s.
    wiiuse_rumble(wm, 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
    wiiuse_rumble(wm, 0);
}

/*! \brief Waits up to \a seconds, or until stopped. */
void wait(double seconds)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (sIsRunning && std::chrono::steady_clock::now() < end)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
}

bool parseCues(const char *text, std::vector<double> *cues)
{
    for (const char *p = text; *p;) {
        char *end = nullptr;
        const double seconds = std::strtod(p, &end);
        if (end == p || seconds <= 0.0)
            return false;
        cues->push_back(seconds);
        p = *end == ',' ? end + 1 : end;
        if (*end != ',' && *end != '\0')
            return false;
    }
    return !cues->empty();
}

void printUsage()
{
    std::printf("usage: wiimosc-record <file.csv> [options]\n"
                "  --seconds <s>      record this long (default: until Ctrl+C)\n"
                "  --cues <s,s,...>   cued steps of these durations: two short buzzes start step 0, one short buzz\n"
                "                     each further step, a long buzz the end; e.g. --cues 6,12,12,15,15\n"
                "  --address <addr>   record the Wiimote Plus with this Bluetooth address (default: the first one)\n"
                "Press a button on the remote to connect it.\n");
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 2 || argv[1][0] == '-') {
        printUsage();
        return 1;
    }
    double seconds = 0.0;
    std::vector<double> cues;
    std::string address;
    for (int i = 2; i < argc; ++i) {
        const bool hasValue = i + 1 < argc;
        if (std::strcmp(argv[i], "--seconds") == 0 && hasValue) {
            seconds = std::atof(argv[++i]);
        } else if (std::strcmp(argv[i], "--address") == 0 && hasValue) {
            address = argv[++i];
        } else if (std::strcmp(argv[i], "--cues") == 0 && hasValue) {
            if (!parseCues(argv[++i], &cues)) {
                printUsage();
                return 1;
            }
        } else {
            printUsage();
            return 1;
        }
    }

    FILE *file = std::fopen(argv[1], "w");
    if (!file) {
        std::fprintf(stderr, "can't write %s\n", argv[1]);
        return 1;
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::signal(SIGINT, [](int) { sIsRunning = false; });
#ifdef SIGBREAK
    std::signal(SIGBREAK, [](int) { sIsRunning = false; });
#endif

    Recorder recorder(file);
    WiimoteHub hub(&recorder);
    if (!address.empty()) {
        // Id 0 is reserved for that address, so only it can get id 0.
        std::string error;
        if (!hub.setFixedId(0, address, &error)) {
            std::fprintf(stderr, "--address: %s\n", error.c_str());
            return 1;
        }
        recorder.setRecordedId(0);
    }
    if (!hub.start(WiimotePairer::Method::SyncButton))
        return 1;
    std::printf("waiting for a Wiimote Plus (Ctrl+C to quit)\n");
    while (sIsRunning && !recorder.remote())
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

    if (sIsRunning && !cues.empty()) {
        wiimote_t *wm = recorder.remote();
        wait(2.0); // the MotionPlus settles, and the connection rumble is over
        buzz(wm, 150);
        wait(0.2);
        buzz(wm, 150);
        for (size_t i = 0; i < cues.size() && sIsRunning; ++i) {
            recorder.markPhase(static_cast<int>(i));
            if (i > 0)
                buzz(wm, 200);
            std::printf("step %zu (%g s), %d reports so far\n", i, cues[i], recorder.count());
            wait(cues[i]);
        }
        if (sIsRunning)
            buzz(wm, 1000);
    } else if (sIsRunning) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
        while (sIsRunning && (seconds <= 0.0 || std::chrono::steady_clock::now() < end))
            wait(seconds <= 0.0 ? 1.0 : 0.1);
    }

    hub.stop();
    std::fclose(file);
    std::printf("%d reports written to %s\n", recorder.count(), argv[1]);
    return 0;
}
