#include "WiimoteSpeaker.h"

#include <wiiuse.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <timeapi.h>
#include <hidsdi.h>
#endif

// Not exported by wiiuse.h, but part of the (static) library: sends a raw output report.
extern "C" int wiiuse_send(struct wiimote_t *wm, byte report_type, byte *msg, int len);

namespace {

using Clock = std::chrono::steady_clock;

// Output reports and registers, see https://wiibrew.org/wiki/Wiimote#Speaker
const uint8_t sReportSpeakerEnable = 0x14;
const uint8_t sReportSpeakerData = 0x18;
const uint8_t sReportSpeakerMute = 0x19;
const unsigned int sRegisterSpeaker = 0x04a20000; //!< 0x04: register space (rather than EEPROM)
const uint8_t sAckRequest = 0x02; //!< in an output report's first byte: the remote answers with 0x22 (report, error)

// Register writes aren't acknowledged before wiiuse_write_data() returns; Settings::setupDelayMs paces them.
const auto sSpeakerPowerDelay = std::chrono::milliseconds(150); //!< the remote acks 0x14 only after ~110 ms
const auto sStatisticsInterval = std::chrono::seconds(5);

// Yamaha ADPCM tables (as in ffmpeg's adpcm_yamaha, and Dolphin's speaker emulation)
const int sDiffLookup[16] = {1, 3, 5, 7, 9, 11, 13, 15, -1, -3, -5, -7, -9, -11, -13, -15};
const int sIndexScale[16] = {230, 230, 230, 230, 307, 409, 512, 614, 230, 230, 230, 230, 307, 409, 512, 614};

double milliseconds(Clock::duration duration)
{
    return std::chrono::duration<double, std::milli>(duration).count();
}

} // namespace

#ifdef _WIN32
/*! \brief Writes output reports without waiting for each to complete, keeping up to a fixed number in flight. */
class WiimoteSpeaker::AsyncWriter
{
public:
    static const int sReportSize = 22; //!< what wiiuse writes, too (report id + 21 bytes)

    AsyncWriter(HANDLE handle, int depth)
        : mHandle(handle)
        , mEntries(std::clamp(depth, 1, 16))
    {
        for (Entry &entry : mEntries)
            entry.overlap.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    }

    ~AsyncWriter()
    {
        for (Entry &entry : mEntries) {
            finish(&entry);
            CloseHandle(entry.overlap.hEvent);
        }
    }

    /*! \brief Queues \a report; waits for the oldest write first if the queue is full. */
    bool write(const uint8_t *report)
    {
        Entry &entry = mEntries[mNext];
        mNext = (mNext + 1) % mEntries.size();
        bool isOk = finish(&entry);

        std::memcpy(entry.data, report, sReportSize);
        ResetEvent(entry.overlap.hEvent);
        if (!WriteFile(mHandle, entry.data, sReportSize, nullptr, &entry.overlap)) {
            if (GetLastError() != ERROR_IO_PENDING)
                return false;
            entry.isPending = true;
        }
        return isOk;
    }

private:
    struct Entry {
        OVERLAPPED overlap = {};
        uint8_t data[sReportSize] = {};
        bool isPending = false;
    };

    /*! \brief Waits for the entry's write to complete (or cancels it after 500 ms, as wiiuse does). */
    bool finish(Entry *entry)
    {
        if (!entry->isPending)
            return true;
        if (WaitForSingleObject(entry->overlap.hEvent, 500) != WAIT_OBJECT_0)
            CancelIoEx(mHandle, &entry->overlap);
        DWORD bytes = 0;
        entry->isPending = false;
        return GetOverlappedResult(mHandle, &entry->overlap, &bytes, TRUE) != 0;
    }

    HANDLE mHandle;
    std::vector<Entry> mEntries;
    size_t mNext = 0;
};
#else
/*! \brief Unused: elsewhere, every writer sends through wiiuse (synchronous IOHIDDeviceSetReport on macOS). */
class WiimoteSpeaker::AsyncWriter
{
};
#endif

const char *WiimoteSpeaker::writerName(Writer writer)
{
    switch (writer) {
    case Writer::Async:
        return "async";
    case Writer::Wiiuse:
        return "wiiuse";
    case Writer::SetOutputReport:
        return "setreport";
    case Writer::Register:
        return "register";
    }
    return "?";
}

WiimoteSpeaker::WiimoteSpeaker(wiimote_t *wm, int id, const Settings &settings, AbstractSoundSource *source)
    : mWiimote(wm)
    , mId(id)
    , mSettings(settings)
    , mSource(source)
{
}

WiimoteSpeaker::~WiimoteSpeaker()
{
    stop();
}

void WiimoteSpeaker::start()
{
    if (mThread.joinable())
        return;
    mIsRunning = true;
    mThread = std::thread(&WiimoteSpeaker::run, this);
}

void WiimoteSpeaker::stop()
{
    mIsRunning = false;
    if (mThread.joinable())
        mThread.join();
}

void WiimoteSpeaker::run()
{
#ifdef _WIN32
    timeBeginPeriod(1); // default sleep granularity is ~15 ms, more than a report period
#endif
    std::this_thread::sleep_for(std::chrono::milliseconds(mSettings.startDelayMs));
    if (mSettings.isRadioBusy && mSettings.isRadioBusy()) {
        std::printf("[%d] waiting for the running scan to finish\n", mId);
        while (mIsRunning && mSettings.isRadioBusy())
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!mIsRunning) {
#ifdef _WIN32
        timeEndPeriod(1);
#endif
        return;
    }
    setUp();

    const int count = samplesPerReport(mSettings.format);
    const auto period = std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(static_cast<double>(count) / mSettings.sampleRate));
    bool isPlaying = false;
    Clock::time_point next;
    Clock::time_point playStart;
    Clock::time_point lastPrint;

    while (mIsRunning && WIIMOTE_IS_CONNECTED(mWiimote)) {
        float samples[40] = {};
        if (mSource->read(samples, count) == 0) {
            if (isPlaying) {
                printStatistics(std::chrono::duration<double>(Clock::now() - playStart).count());
                isPlaying = false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        Clock::time_point now = Clock::now();
        if (!isPlaying) {
            isPlaying = true;
            mStatistics = Statistics{};
            next = playStart = lastPrint = now;
        } else if (now - next > period) {
            // Fell behind: don't burst to catch up, the speaker's buffer is tiny. Skip the missed samples instead,
            // so a sound keeps its duration even when the link can't carry all of it.
            ++mStatistics.resyncs;
            skip(static_cast<int>((now - next) / period) * count);
            next = now;
        } else {
            mStatistics.maxLateMs = std::max(mStatistics.maxLateMs, milliseconds(now - next));
        }

        uint8_t data[21] = {};
        data[0] = static_cast<uint8_t>(20 << 3); // payload length
        if (mStatistics.reports < mSettings.ackReports)
            data[0] |= sAckRequest;
        encode(samples, data + 1);
        if (mStatistics.reports < std::min(mSettings.ackReports, 3)) {
            std::printf("[%d] audio report:", mId);
            for (uint8_t byte : data)
                std::printf(" %02x", byte);
            std::printf("\n");
        }
        const Clock::time_point writeStart = Clock::now();
        if (!sendAudio(data))
            ++mStatistics.failedWrites;
        const double writeMs = milliseconds(Clock::now() - writeStart);
        ++mStatistics.reports;
        mStatistics.totalWriteMs += writeMs;
        mStatistics.maxWriteMs = std::max(mStatistics.maxWriteMs, writeMs);

        now = Clock::now();
        if (now - lastPrint >= sStatisticsInterval) {
            printStatistics(std::chrono::duration<double>(now - playStart).count());
            lastPrint = now;
        }
        next += period;
        std::this_thread::sleep_until(next);
    }

    if (isPlaying)
        printStatistics(std::chrono::duration<double>(Clock::now() - playStart).count());
#ifdef _WIN32
    mWriter.reset(); // waits for the queued reports
#endif
    switchOff();
#ifdef _WIN32
    timeEndPeriod(1);
#endif
}

void WiimoteSpeaker::setUp()
{
    const bool isAdpcm = mSettings.format == Format::Adpcm;
    // The rate register holds a divisor; the volume's range differs per format.
    const double dividend = isAdpcm ? 6000000.0 : 12000000.0;
    const int rateValue = std::clamp(static_cast<int>(std::lround(dividend / mSettings.sampleRate)), 1, 0xffff);
    const int maxVolume = 0x40; // more is no louder for PCM, and silent from 0x80 (measured; WiiBrew says 0xff)
    const int volume = std::clamp(static_cast<int>(std::lround(mSettings.volume * maxVolume)), 0, maxVolume);

    const uint8_t ack = mSettings.ackReports > 0 ? sAckRequest : 0x00;
    const uint8_t on = 0x04 | ack;
    const uint8_t off = 0x00 | ack;
    sendReport(sReportSpeakerEnable, &on, 1);
    std::this_thread::sleep_for(std::max(sSpeakerPowerDelay, setupDelay()));
    sendReport(sReportSpeakerMute, &on, 1);
    std::this_thread::sleep_for(setupDelay());
    const uint8_t one = 0x01;
    const uint8_t eight = 0x08;
    writeRegister(sRegisterSpeaker + 0x09, &one, 1);
    writeRegister(sRegisterSpeaker + 0x01, &eight, 1);
    const uint8_t config[7] = {0x00,
                               static_cast<uint8_t>(isAdpcm ? 0x00 : 0x40),
                               static_cast<uint8_t>(rateValue & 0xff),
                               static_cast<uint8_t>(rateValue >> 8),
                               static_cast<uint8_t>(volume),
                               0x00,
                               0x00};
    writeRegister(sRegisterSpeaker + 0x01, config, sizeof(config));
    writeRegister(sRegisterSpeaker + 0x08, &one, 1);
    sendReport(sReportSpeakerMute, &off, 1);
    std::this_thread::sleep_for(setupDelay());

    // Diagnostics: the status report says whether the speaker is on (printed by the listener when it arrives on the
    // device thread). Reading the registers back isn't useful: they read as all zeros right after being written.
    wiiuse_status(mWiimote);

    mPredictor = 0;
    mStep = 127;
#ifdef _WIN32
    if (mSettings.writer == Writer::Async)
        mWriter = std::make_unique<AsyncWriter>(mWiimote->dev_handle, mSettings.queueDepth);
#endif
    const int count = samplesPerReport(mSettings.format);
    std::printf("[%d] speaker on: %s, %.0f Hz, volume %d/%d, %.1f reports/s, writer %s, setup delay %d ms\n", mId,
                isAdpcm ? "ADPCM" : "8-bit PCM", dividend / rateValue, volume, maxVolume, static_cast<double>(mSettings.sampleRate) / count,
                writerName(mSettings.writer), mSettings.setupDelayMs);
}

void WiimoteSpeaker::switchOff()
{
    const uint8_t on = 0x04;
    const uint8_t off = 0x00;
    sendReport(sReportSpeakerMute, &on, 1);
    sendReport(sReportSpeakerEnable, &off, 1);
}

bool WiimoteSpeaker::sendReport(uint8_t reportType, const uint8_t *data, int length)
{
    uint8_t buffer[21] = {};
    std::memcpy(buffer, data, std::min<size_t>(length, sizeof(buffer)));
    // Bit 0 of every output report is the rumble motor: keep it as it is.
    if (WIIMOTE_IS_SET(mWiimote, WIIMOTE_STATE_RUMBLE))
        buffer[0] |= 0x01;
    return wiiuse_send(mWiimote, reportType, buffer, length) != 0;
}

bool WiimoteSpeaker::sendAudio(const uint8_t *data)
{
    if (mSettings.writer == Writer::Register) {
        // A write carries at most 16 bytes; the speaker data register works as a FIFO.
        return wiiuse_write_data(mWiimote, sRegisterSpeaker, data + 1, 16) != 0
            && wiiuse_write_data(mWiimote, sRegisterSpeaker, data + 17, 4) != 0;
    }
#ifdef _WIN32
    if (mSettings.writer == Writer::Wiiuse)
        return sendReport(sReportSpeakerData, data, 21);
    if (!WIIMOTE_IS_CONNECTED(mWiimote))
        return false;
    uint8_t report[AsyncWriter::sReportSize] = {sReportSpeakerData};
    std::memcpy(report + 1, data, 21);
    if (WIIMOTE_IS_SET(mWiimote, WIIMOTE_STATE_RUMBLE))
        report[1] |= 0x01;
    if (mSettings.writer == Writer::SetOutputReport)
        return HidD_SetOutputReport(mWiimote->dev_handle, report, sizeof(report)) != 0;
    return mWriter->write(report);
#else
    return sendReport(sReportSpeakerData, data, 21);
#endif
}

void WiimoteSpeaker::skip(int count)
{
    float samples[40];
    while (count > 0) {
        const int chunk = std::min(count, 40);
        if (mSource->read(samples, chunk) < chunk)
            return;
        count -= chunk;
    }
}

void WiimoteSpeaker::writeRegister(unsigned int address, const uint8_t *data, int length)
{
    wiiuse_write_data(mWiimote, address, data, static_cast<byte>(length));
    std::this_thread::sleep_for(setupDelay());
}

void WiimoteSpeaker::encode(const float *samples, uint8_t *data)
{
    if (mSettings.format == Format::Pcm8) {
        for (int i = 0; i < 20; ++i) {
            const float sample = std::clamp(samples[i], -1.0f, 1.0f);
            data[i] = static_cast<uint8_t>(static_cast<int8_t>(std::lround(sample * 127.0f)));
        }
        return;
    }

    // Two samples per byte, high nibble first.
    for (int i = 0; i < 40; ++i) {
        const int sample = static_cast<int>(std::lround(std::clamp(samples[i], -1.0f, 1.0f) * 32767.0f));
        const int delta = sample - mPredictor;
        const int nibble = std::min(7, std::abs(delta) * 4 / mStep) + (delta < 0 ? 8 : 0);
        mPredictor = std::clamp(mPredictor + mStep * sDiffLookup[nibble] / 8, -32768, 32767);
        mStep = std::clamp((mStep * sIndexScale[nibble]) >> 8, 127, 24576);
        if (i % 2 == 0)
            data[i / 2] = static_cast<uint8_t>(nibble << 4);
        else
            data[i / 2] |= static_cast<uint8_t>(nibble);
    }
}

void WiimoteSpeaker::printStatistics(double seconds)
{
    const Statistics &s = mStatistics;
    std::printf("[%d] speaker: %.1f s, %d reports (%.1f/s), write avg %.1f ms / max %.1f ms, max late %.1f ms, "
                "%d resyncs, %d failed writes\n",
                mId, seconds, s.reports, seconds > 0.0 ? s.reports / seconds : 0.0,
                s.reports ? s.totalWriteMs / s.reports : 0.0, s.maxWriteMs, s.maxLateMs, s.resyncs, s.failedWrites);
}
