/*! \brief Speaker test tool: pairs devices like wiimosc-pair, and plays a sine, a file, or a clip on one Wiimote. */

#include "Audio.h"
#include "SoundSources.h"
#include "WiimoteSpeaker.h"
#include "hub/WiimoteHub.h"

#include <wiiuse.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

std::atomic<bool> sIsRunning{true};

const double sClipTailSeconds = 0.1; //!< silence after a clip, lets the ADPCM predictor settle back to 0

const int sSweepStepMs = 6000;      //!< how long each sweep variant plays

enum class TestMode { Sine, File, Trigger, Sweep };

/*! \brief Plays a clip from the start on each restart(); silent otherwise. */
class ClipSource : public AbstractSoundSource
{
public:
    explicit ClipSource(std::vector<float> samples)
        : mSamples(std::move(samples))
    {
    }

    void restart()
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mPosition = 0;
        mIsPlaying = true;
    }

    /*! \brief True once the clip played to its end (and wasn't restarted since). */
    bool hasFinished() const { return mHasFinished; }

    int read(float *samples, int count) override
    {
        std::lock_guard<std::mutex> lock(mMutex);
        if (!mIsPlaying)
            return 0;
        const int available = static_cast<int>(mSamples.size() - mPosition);
        const int filled = std::min(count, available);
        std::copy(mSamples.begin() + mPosition, mSamples.begin() + mPosition + filled, samples);
        mPosition += filled;
        if (mPosition >= mSamples.size()) {
            mIsPlaying = false;
            mHasFinished = true;
        } else {
            mHasFinished = false;
        }
        return filled;
    }

private:
    std::mutex mMutex;
    std::vector<float> mSamples;
    size_t mPosition = 0;
    bool mIsPlaying = false;
    std::atomic<bool> mHasFinished{false};
};

/*! \brief Runs a WiimoteSpeaker on the target remote while it's connected. */
class SpeakerTester : public AbstractHubListener
{
public:
    SpeakerTester(int targetId, TestMode mode, const WiimoteSpeaker::Settings &settings,
                  AbstractSoundSource *source, ClipSource *clip)
        : mTargetId(targetId)
        , mMode(mode)
        , mSettings(settings)
        , mSource(source)
        , mClip(clip)
    {
    }

    void connected(int id, wiimote_t *wm, DeviceType type) override
    {
        if (id != mTargetId)
            return;
        if (type == DeviceType::BalanceBoard) {
            std::printf("[%d] the Balance Board has no speaker\n", id);
            return;
        }
        mIsTargetConnected = true;
        if (mMode == TestMode::Sweep) {
            stopSweep();
            mIsSweeping = true;
            mSweepThread = std::thread(&SpeakerTester::runSweep, this, id, wm);
            return;
        }
        std::lock_guard<std::mutex> lock(mMutex);
        mSpeaker = std::make_unique<WiimoteSpeaker>(wm, id, mSettings, mSource);
        if (mMode == TestMode::File)
            mClip->restart();
        mSpeaker->start();
    }

    void disconnected(int id, DeviceType) override
    {
        if (id == mTargetId) {
            stop();
            mIsTargetConnected = false;
        }
    }

    /*! \brief Makes speakers started from now on wait while \a isRadioBusy returns true, before their setup. */
    void setRadioBusyCheck(std::function<bool()> isRadioBusy) { mSettings.isRadioBusy = std::move(isRadioBusy); }

    /*! \brief True while the target remote is connected (and is a Wiimote). */
    bool isTargetConnected() const { return mIsTargetConnected; }

    void event(int id, wiimote_t *wm) override
    {
        if (id != mTargetId)
            return;
        if (wm->event == WIIUSE_STATUS) {
            // wiiuse only ever sets this flag, so it shows whether the speaker was reported on at least once.
            std::printf("[%d] status: speaker %s, battery %.2f\n", id,
                        WIIUSE_USING_SPEAKER(wm) ? "enabled" : "not enabled", wm->battery_level);
        }
        if (mMode != TestMode::Trigger)
            return;
        if (wm->event == WIIUSE_EVENT && IS_JUST_PRESSED(wm, WIIMOTE_BUTTON_A)) {
            std::printf("[%d] A pressed, playing\n", id);
            mClip->restart();
        }
    }

    /*! \brief Stops the speaker; the hub doesn't call disconnected() for devices still connected on shutdown. */
    void stop()
    {
        stopSweep();
        std::lock_guard<std::mutex> lock(mMutex);
        mSpeaker.reset();
    }

    /*! \brief True once a sweep played all its variants. */
    bool hasSweepFinished() const { return mHasSweepFinished; }

private:
    void stopSweep()
    {
        mIsSweeping = false;
        if (mSweepThread.joinable())
            mSweepThread.join();
    }

    /*! \brief Plays the sine with each writer, with a normal and a slow setup, announcing each variant. */
    void runSweep(int id, wiimote_t *wm)
    {
        const WiimoteSpeaker::Writer writers[] = {WiimoteSpeaker::Writer::Async, WiimoteSpeaker::Writer::Wiiuse,
                                                  WiimoteSpeaker::Writer::SetOutputReport};
        const int setupDelays[] = {30, 200};
        const int count = 6;
        int number = 0;
        for (int setupDelay : setupDelays) {
            for (WiimoteSpeaker::Writer writer : writers) {
                if (!mIsSweeping)
                    return;
                WiimoteSpeaker::Settings settings = mSettings;
                settings.writer = writer;
                settings.setupDelayMs = setupDelay;
                std::printf("[%d] ===== variant %d/%d: writer %s, setup delay %d ms =====\n", id, ++number, count,
                            WiimoteSpeaker::writerName(writer), setupDelay);
                {
                    std::lock_guard<std::mutex> lock(mMutex);
                    mSpeaker = std::make_unique<WiimoteSpeaker>(wm, id, settings, mSource);
                    mSpeaker->start();
                }
                for (int i = 0; i < sSweepStepMs / 100 && mIsSweeping; ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                {
                    std::lock_guard<std::mutex> lock(mMutex);
                    mSpeaker.reset();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            }
        }
        std::printf("[%d] sweep finished\n", id);
        mHasSweepFinished = true;
    }

    std::thread mSweepThread;
    std::atomic<bool> mIsSweeping{false};
    std::atomic<bool> mHasSweepFinished{false};
    std::atomic<bool> mIsTargetConnected{false};

    int mTargetId;
    TestMode mMode;
    WiimoteSpeaker::Settings mSettings;
    AbstractSoundSource *mSource;
    ClipSource *mClip;
    std::mutex mMutex; //!< guards mSpeaker
    std::unique_ptr<WiimoteSpeaker> mSpeaker;
};

void printUsage()
{
    std::printf(
        "usage: wiimosc-speaker [--target-id <id>] [--test-mode sine|file|trigger|sweep] [--file <path>] [options]\n"
        "  --target-id <id>       remote to play on (default 0)\n"
        "  --test-mode <mode>     sine:    plays a sine wave until Ctrl+C (default)\n"
        "                         file:    streams --file once, then quits\n"
        "                         trigger: plays --file from the start on every press of A\n"
        "                         sweep:   plays the sine for 6 s with each writer, with a normal and a slow\n"
        "                                  setup (6 variants, announced), then quits\n"
        "  --file <path>          audio file (wav, mp3, m4a, ... anything Windows can decode)\n"
        "  --frequency <hz>       sine frequency (default 1000)\n"
        "  --format adpcm|pcm     4-bit Yamaha ADPCM (default) or 8-bit PCM\n"
        "  --rate <hz>            sample rate (default 6000 for ADPCM, 3000 for PCM: the speaker's own rates)\n"
        "  --volume <0-1>         speaker volume (default 1: 0x40)\n"
        "  --writer <writer>      how audio reports are sent: async (default), wiiuse, setreport, register\n"
        "  --queue <1-16>         audio reports in flight at once for async (default 3)\n"
        "  --setup-delay <ms>     pause after each speaker setup step (default 30)\n"
        "  --start-delay <ms>     pause before the speaker setup (default 0)\n"
        "  --ack <n>              diagnostics: requests acks for the speaker setup and the first n audio reports\n"
        "                         (printed if the environment variable WIIUSE_LOG_ACKS is set)\n"
        "  --keep-scanning        keeps scanning for remotes to pair while the target is connected (by default,\n"
        "                         scanning pauses then: an inquiry costs the link throughput)\n"
        "  --one-two, --no-auth   pairing method, as for wiimosc-pair\n"
        "  --shutdown-on-exit     switches off all connected devices when quitting\n"
        "  --idle-timeout <min>   switches off devices that have been idle this long (default 0 = never)\n"
        "  --scan-timeout <s>     stops scanning for remotes to pair after this long (default 0 = never)\n"
        "  --id <id>=<address>    always gives the device with this Bluetooth address this id; repeatable\n"
        "Options take their value as \"--option value\" or \"--option=value\".\n");
}

} // namespace

int main(int argc, char **argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    int targetId = 0;
    TestMode mode = TestMode::Sine;
    std::string path;
    double frequency = 1000.0; // the speaker barely reproduces much below ~600 Hz
    int rate = 0;
    WiimoteSpeaker::Settings settings;
    WiimotePairer::Method method = WiimotePairer::Method::SyncButton;
    std::vector<std::string> fixedIds;
    bool isShuttingDownOnExit = false;
    bool isKeepingScanning = false;
    double idleTimeout = 0.0;
    double scanTimeout = 0.0;

    for (int i = 1; i < argc; ++i) {
        std::string name = argv[i];
        std::string value;
        bool hasValue = false;
        const size_t equals = name.find('=');
        if (name.rfind("--", 0) == 0 && equals != std::string::npos) {
            value = name.substr(equals + 1);
            name = name.substr(0, equals);
            hasValue = true;
        }
        // Takes the value from "--option=value" or from the next argument.
        auto takeValue = [&]() {
            if (!hasValue && i + 1 < argc) {
                value = argv[++i];
                hasValue = true;
            }
            return hasValue;
        };

        bool isValid = true;
        if (name == "--one-two") {
            method = WiimotePairer::Method::OneTwo;
        } else if (name == "--idle-timeout" && takeValue()) {
            idleTimeout = std::atof(value.c_str());
            isValid = idleTimeout >= 0.0;
        } else if (name == "--scan-timeout" && takeValue()) {
            scanTimeout = std::atof(value.c_str());
            isValid = scanTimeout >= 0.0;
        } else if (name == "--keep-scanning") {
            isKeepingScanning = true;
        } else if (name == "--shutdown-on-exit") {
            isShuttingDownOnExit = true;
        } else if (name == "--no-auth") {
            method = WiimotePairer::Method::None;
        } else if (name == "--target-id" && takeValue()) {
            targetId = std::atoi(value.c_str());
            isValid = targetId >= 0 && targetId < WiimoteHub::sMaxDevices;
        } else if (name == "--test-mode" && takeValue()) {
            if (value == "sine")
                mode = TestMode::Sine;
            else if (value == "file")
                mode = TestMode::File;
            else if (value == "trigger")
                mode = TestMode::Trigger;
            else if (value == "sweep")
                mode = TestMode::Sweep;
            else
                isValid = false;
        } else if (name == "--file" && takeValue()) {
            path = value;
        } else if (name == "--frequency" && takeValue()) {
            frequency = std::atof(value.c_str());
            isValid = frequency > 0.0;
        } else if (name == "--format" && takeValue()) {
            if (value == "adpcm")
                settings.format = WiimoteSpeaker::Format::Adpcm;
            else if (value == "pcm")
                settings.format = WiimoteSpeaker::Format::Pcm8;
            else
                isValid = false;
        } else if (name == "--rate" && takeValue()) {
            rate = std::atoi(value.c_str());
            isValid = rate >= 200 && rate <= 48000;
        } else if (name == "--volume" && takeValue()) {
            settings.volume = static_cast<float>(std::atof(value.c_str()));
            isValid = settings.volume >= 0.0f && settings.volume <= 1.0f;
        } else if (name == "--writer" && takeValue()) {
            if (value == "async")
                settings.writer = WiimoteSpeaker::Writer::Async;
            else if (value == "wiiuse")
                settings.writer = WiimoteSpeaker::Writer::Wiiuse;
            else if (value == "setreport")
                settings.writer = WiimoteSpeaker::Writer::SetOutputReport;
            else if (value == "register")
                settings.writer = WiimoteSpeaker::Writer::Register;
            else
                isValid = false;
        } else if (name == "--setup-delay" && takeValue()) {
            settings.setupDelayMs = std::atoi(value.c_str());
            isValid = settings.setupDelayMs >= 0 && settings.setupDelayMs <= 2000;
        } else if (name == "--queue" && takeValue()) {
            settings.queueDepth = std::atoi(value.c_str());
            isValid = settings.queueDepth >= 1 && settings.queueDepth <= 16;
        } else if (name == "--start-delay" && takeValue()) {
            settings.startDelayMs = std::atoi(value.c_str());
            isValid = settings.startDelayMs >= 0 && settings.startDelayMs <= 10000;
        } else if (name == "--ack" && takeValue()) {
            settings.ackReports = std::atoi(value.c_str());
            isValid = settings.ackReports >= 0;
        } else if (name == "--id" && takeValue()) {
            fixedIds.push_back(value);
        } else {
            printUsage();
            return 1;
        }
        if (!isValid) {
            std::fprintf(stderr, "%s: invalid value \"%s\"\n", name.c_str(), value.c_str());
            return 1;
        }
    }
    settings.sampleRate = rate > 0 ? rate : (settings.format == WiimoteSpeaker::Format::Adpcm ? 6000 : 3000);
    const bool isPlayingSine = mode == TestMode::Sine || mode == TestMode::Sweep;
    if (!isPlayingSine && path.empty()) {
        std::fprintf(stderr, "--test-mode file and trigger need --file\n");
        return 1;
    }

    // Load the file up front, so playback never waits for decoding.
    std::vector<float> clipSamples;
    if (!isPlayingSine) {
        std::string error;
        if (!loadAudioFile(path, settings.sampleRate, &clipSamples, &error)) {
            std::fprintf(stderr, "%s: %s\n", path.c_str(), error.c_str());
            return 1;
        }
        normalize(&clipSamples, sSoundPeak);
        std::printf("loaded %s: %.1f s at %d Hz\n", path.c_str(),
                    static_cast<double>(clipSamples.size()) / settings.sampleRate, settings.sampleRate);
        clipSamples.resize(clipSamples.size() + static_cast<size_t>(sClipTailSeconds * settings.sampleRate), 0.0f);
    }
    SineSource sine(frequency, settings.sampleRate);
    ClipSource clip(std::move(clipSamples));
    AbstractSoundSource *source = isPlayingSine ? static_cast<AbstractSoundSource *>(&sine) : &clip;

    SpeakerTester tester(targetId, mode, settings, source, &clip);
    WiimoteHub hub(&tester);
    for (const std::string &argument : fixedIds) {
        std::string error;
        if (!hub.parseFixedId(argument, &error)) {
            std::fprintf(stderr, "--id: %s\n", error.c_str());
            return 1;
        }
    }

    std::signal(SIGINT, [](int) { sIsRunning = false; });
#ifdef SIGBREAK
    std::signal(SIGBREAK, [](int) { sIsRunning = false; }); // Ctrl+Break (Windows)
#endif

    if (!isKeepingScanning)
        tester.setRadioBusyCheck([&hub] { return !hub.isPairingIdle(); });
    hub.setShutdownOnExit(isShuttingDownOnExit);
    hub.setIdleTimeout(idleTimeout);
    hub.setScanTimeout(scanTimeout);
    if (!hub.start(method))
        return 1;
    std::printf("scanning, press %s on a remote to pair it; playing on [%d] once it connects%s (Ctrl+C to quit)\n",
                method == WiimotePairer::Method::SyncButton ? "SYNC" : "1+2", targetId,
                mode == TestMode::Trigger ? ", on every press of A" : "");

    bool isPaused = false;
    while (sIsRunning && !(mode == TestMode::File && clip.hasFinished()) && !tester.hasSweepFinished()) {
        const bool isPausing = !isKeepingScanning && tester.isTargetConnected();
        if (isPausing != isPaused) {
            isPaused = isPausing;
            hub.setPairingPaused(isPaused);
            std::printf("%s scanning for remotes\n", isPaused ? "paused" : "resumed");
        }
        WiimotePairer::processEvents(100);
    }
    if (mode == TestMode::File && clip.hasFinished())
        std::printf("[%d] file finished\n", targetId);
    tester.stop();
    hub.stop();
    return 0;
}
