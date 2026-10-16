#include "Audio.h"

#include <windows.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <cstdio>

using Microsoft::WRL::ComPtr;

namespace {

std::string hresultText(const char *what, HRESULT result)
{
    char text[128];
    std::snprintf(text, sizeof(text), "%s failed (0x%08lx)", what, static_cast<unsigned long>(result));
    return text;
}

/*! \brief Decodes the first audio stream of \a path into interleaved float samples. */
bool decode(const std::wstring &path, std::vector<float> *samples, int *channels, int *sampleRate,
            std::string *error)
{
    ComPtr<IMFSourceReader> reader;
    HRESULT result = MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader);
    if (FAILED(result)) {
        *error = hresultText("opening the file", result);
        return false;
    }
    const DWORD stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    result = reader->SetStreamSelection(stream, TRUE);
    if (FAILED(result)) {
        *error = "the file has no audio stream";
        return false;
    }

    // Asking for float PCM makes the reader insert the decoder; channels and rate stay as they are.
    ComPtr<IMFMediaType> requested;
    MFCreateMediaType(&requested);
    requested->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    requested->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
    result = reader->SetCurrentMediaType(stream, nullptr, requested.Get());
    if (FAILED(result)) {
        *error = hresultText("decoding to float", result);
        return false;
    }
    ComPtr<IMFMediaType> actual;
    reader->GetCurrentMediaType(stream, &actual);
    *channels = static_cast<int>(MFGetAttributeUINT32(actual.Get(), MF_MT_AUDIO_NUM_CHANNELS, 0));
    *sampleRate = static_cast<int>(MFGetAttributeUINT32(actual.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 0));
    if (*channels <= 0 || *sampleRate <= 0) {
        *error = "unknown audio format";
        return false;
    }

    for (;;) {
        DWORD flags = 0;
        ComPtr<IMFSample> sample;
        result = reader->ReadSample(stream, 0, nullptr, &flags, nullptr, &sample);
        if (FAILED(result)) {
            *error = hresultText("reading", result);
            return false;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
            break;
        if (!sample)
            continue;

        ComPtr<IMFMediaBuffer> buffer;
        sample->ConvertToContiguousBuffer(&buffer);
        BYTE *data = nullptr;
        DWORD size = 0;
        if (SUCCEEDED(buffer->Lock(&data, nullptr, &size))) {
            const float *floats = reinterpret_cast<const float *>(data);
            samples->insert(samples->end(), floats, floats + size / sizeof(float));
            buffer->Unlock();
        }
    }
    return true;
}

} // namespace

bool loadAudioFile(const std::string &path, int sampleRate, std::vector<float> *samples, std::string *error)
{
    // Command-line arguments are in the ANSI code page; Media Foundation wants an absolute wide path.
    wchar_t widePath[MAX_PATH];
    wchar_t fullPath[MAX_PATH];
    if (!MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, widePath, MAX_PATH)
        || !GetFullPathNameW(widePath, MAX_PATH, fullPath, nullptr)) {
        *error = "invalid path";
        return false;
    }
    if (GetFileAttributesW(fullPath) == INVALID_FILE_ATTRIBUTES) {
        *error = "file not found";
        return false;
    }

    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);
    std::vector<float> interleaved;
    int channels = 0;
    int fileRate = 0;
    const bool isDecoded = decode(fullPath, &interleaved, &channels, &fileRate, error);
    MFShutdown();
    if (SUCCEEDED(comResult))
        CoUninitialize();
    if (!isDecoded)
        return false;

    std::vector<float> mono(interleaved.size() / channels);
    for (size_t i = 0; i < mono.size(); ++i) {
        float sum = 0.0f;
        for (int c = 0; c < channels; ++c)
            sum += interleaved[i * channels + c];
        mono[i] = sum / channels;
    }
    *samples = resample(mono, fileRate, sampleRate);
    return true;
}
