#include "Audio.h"

#include <AudioToolbox/AudioToolbox.h>

#include <cstdio>

#include <unistd.h>

namespace {

std::string statusText(const char *what, OSStatus status)
{
    char text[128];
    std::snprintf(text, sizeof(text), "%s failed (%d)", what, static_cast<int>(status));
    return text;
}

/*! \brief Decodes \a file into interleaved float samples, keeping its channels and rate. */
bool decode(ExtAudioFileRef file, std::vector<float> *samples, int *channels, int *sampleRate, std::string *error)
{
    AudioStreamBasicDescription fileFormat = {};
    UInt32 size = sizeof(fileFormat);
    OSStatus status = ExtAudioFileGetProperty(file, kExtAudioFileProperty_FileDataFormat, &size, &fileFormat);
    if (status != noErr) {
        *error = statusText("reading the format", status);
        return false;
    }
    *channels = static_cast<int>(fileFormat.mChannelsPerFrame);
    *sampleRate = static_cast<int>(fileFormat.mSampleRate);
    if (*channels <= 0 || *sampleRate <= 0) {
        *error = "unknown audio format";
        return false;
    }

    // Asking for float PCM makes ExtAudioFile insert the decoder.
    AudioStreamBasicDescription clientFormat = {};
    clientFormat.mSampleRate = fileFormat.mSampleRate;
    clientFormat.mFormatID = kAudioFormatLinearPCM;
    clientFormat.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    clientFormat.mChannelsPerFrame = fileFormat.mChannelsPerFrame;
    clientFormat.mBitsPerChannel = 32;
    clientFormat.mFramesPerPacket = 1;
    clientFormat.mBytesPerFrame = 4 * clientFormat.mChannelsPerFrame;
    clientFormat.mBytesPerPacket = clientFormat.mBytesPerFrame;
    status = ExtAudioFileSetProperty(file, kExtAudioFileProperty_ClientDataFormat, sizeof(clientFormat),
                                     &clientFormat);
    if (status != noErr) {
        *error = statusText("decoding to float", status);
        return false;
    }

    const UInt32 chunkFrames = 4096;
    std::vector<float> chunk(chunkFrames * *channels);
    for (;;) {
        AudioBufferList buffers = {};
        buffers.mNumberBuffers = 1;
        buffers.mBuffers[0].mNumberChannels = clientFormat.mChannelsPerFrame;
        buffers.mBuffers[0].mDataByteSize = static_cast<UInt32>(chunk.size() * sizeof(float));
        buffers.mBuffers[0].mData = chunk.data();
        UInt32 frames = chunkFrames;
        status = ExtAudioFileRead(file, &frames, &buffers);
        if (status != noErr) {
            *error = statusText("reading", status);
            return false;
        }
        if (frames == 0)
            break;
        samples->insert(samples->end(), chunk.begin(), chunk.begin() + frames * *channels);
    }
    return true;
}

} // namespace

bool loadAudioFile(const std::string &path, int sampleRate, std::vector<float> *samples, std::string *error)
{
    if (access(path.c_str(), R_OK) != 0) {
        *error = "file not found";
        return false;
    }
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(path.c_str()), static_cast<CFIndex>(path.size()), false);
    if (!url) {
        *error = "invalid path";
        return false;
    }
    ExtAudioFileRef file = nullptr;
    const OSStatus status = ExtAudioFileOpenURL(url, &file);
    CFRelease(url);
    if (status != noErr) {
        *error = statusText("opening the file", status);
        return false;
    }

    std::vector<float> interleaved;
    int channels = 0;
    int fileRate = 0;
    const bool isDecoded = decode(file, &interleaved, &channels, &fileRate, error);
    ExtAudioFileDispose(file);
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
