#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>

struct wiimote_t;

/*! \brief Delivers the samples a WiimoteSpeaker plays. Called from the speaker's thread. */
class AbstractSoundSource
{
public:
    virtual ~AbstractSoundSource() = default;

    /*! \brief Fills up to \a count samples (-1..1), and returns how many were filled.

        Returning less than \a count means the sound ended (the rest is played as silence); returning 0 means there
        is nothing to play right now, and no reports are sent until there is.
     */
    virtual int read(float *samples, int count) = 0;
};

/*! \brief Streams audio to a Wiimote's speaker, on its own thread.

    The speaker plays 4-bit Yamaha ADPCM (40 samples per report) or 8-bit signed PCM (20 samples per report) at a
    configurable rate. It has a tiny buffer and no flow control, so reports have to arrive at a steady pace: the
    thread sends one report per period (e.g. every 20 ms for ADPCM at 2000 Hz), and prints timing statistics.

    On Windows, a write to the remote takes ~15 ms to complete, so audio reports are written asynchronously, with up
    to Settings::queueDepth in flight. That doesn't raise the ceiling, though: the link carries ~61 reports/s at most
    (measured on the MS stack), which limits ADPCM to ~2400 Hz and PCM to ~1200 Hz.
 */
class WiimoteSpeaker
{
public:
    enum class Format {
        Adpcm, //!< 4-bit Yamaha ADPCM
        Pcm8   //!< 8-bit signed PCM
    };

    /*! \brief How audio reports are sent (an experiment: the speaker hisses, but stays silent). */
    enum class Writer {
        Async,          //!< own overlapped WriteFile, up to queueDepth in flight (Windows)
        Wiiuse,         //!< wiiuse_send(), the path LEDs and rumble take: WriteFile, waiting for each
        SetOutputReport //!< HidD_SetOutputReport(): the HID control channel instead of the interrupt channel (Windows)
    };

    struct Settings {
        Format format = Format::Adpcm;
        int sampleRate = 2000;
        float volume = 1.0f; //!< 0..1, of the format's maximum register value (0x40 for ADPCM, 0xff for PCM)
        int queueDepth = 3;  //!< audio reports in flight at once (Writer::Async); 1 waits for each write
        Writer writer = Writer::Async;
        int setupDelayMs = 30; //!< pause after each setup step (register writes aren't acknowledged synchronously)
    };

    static const char *writerName(Writer writer);

    WiimoteSpeaker(wiimote_t *wm, int id, const Settings &settings, AbstractSoundSource *source);
    ~WiimoteSpeaker();

    WiimoteSpeaker(const WiimoteSpeaker &) = delete;
    WiimoteSpeaker &operator=(const WiimoteSpeaker &) = delete;

    static int samplesPerReport(Format format) { return format == Format::Adpcm ? 40 : 20; }

    /*! \brief Starts the thread, which configures the speaker and then plays the source. */
    void start();

    /*! \brief Stops the thread, and mutes and switches off the speaker (if still connected). */
    void stop();

private:
    struct Statistics {
        int reports = 0;
        int failedWrites = 0;
        int resyncs = 0;          //!< times the thread fell behind by more than a report, and restarted the clock
        double totalWriteMs = 0.0; //!< time spent sending, including waits for a free queue entry
        double maxWriteMs = 0.0;
        double maxLateMs = 0.0;   //!< how much later than scheduled a report was sent
    };

    class AsyncWriter;

    void run();
    bool sendAudio(const uint8_t *data);
    void setUp();
    void switchOff();
    bool sendReport(uint8_t reportType, const uint8_t *data, int length);
    void writeRegister(unsigned int address, const uint8_t *data, int length);
    void encode(const float *samples, uint8_t *data);
    std::chrono::milliseconds setupDelay() const { return std::chrono::milliseconds(mSettings.setupDelayMs); }
    void printStatistics(double seconds);

    wiimote_t *mWiimote;
    int mId;
    Settings mSettings;
    AbstractSoundSource *mSource;
    std::unique_ptr<AsyncWriter> mWriter; //!< Windows only
    std::thread mThread;
    std::atomic<bool> mIsRunning{false};

    // Yamaha ADPCM encoder state; must match the speaker's decoder, which starts over on setUp().
    int mPredictor = 0;
    int mStep = 127;

    Statistics mStatistics;
};
