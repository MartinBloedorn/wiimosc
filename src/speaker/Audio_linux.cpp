#include "Audio.h"

#include <sndfile.h>

#include <algorithm>

bool loadAudioFile(const std::string &path, int sampleRate, std::vector<float> *samples, std::string *error)
{
    SF_INFO info = {};
    SNDFILE *file = sf_open(path.c_str(), SFM_READ, &info);
    if (!file) {
        *error = sf_strerror(nullptr);
        return false;
    }
    if (info.channels <= 0 || info.samplerate <= 0) {
        sf_close(file);
        *error = "unknown audio format";
        return false;
    }

    // libsndfile converts whatever the file holds to float
    const sf_count_t chunkFrames = 4096;
    std::vector<float> chunk(static_cast<size_t>(chunkFrames * info.channels));
    std::vector<float> mono;
    for (sf_count_t frames; (frames = sf_readf_float(file, chunk.data(), chunkFrames)) > 0;) {
        for (sf_count_t frame = 0; frame < frames; ++frame) {
            float sum = 0.0f;
            for (int channel = 0; channel < info.channels; ++channel)
                sum += chunk[static_cast<size_t>(frame * info.channels + channel)];
            mono.push_back(sum / info.channels);
        }
    }
    sf_close(file);

    if (mono.empty()) {
        *error = "no audio data";
        return false;
    }
    *samples = resample(mono, info.samplerate, sampleRate);
    return true;
}
