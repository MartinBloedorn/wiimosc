#pragma once

#include <string>
#include <vector>

/*! \brief Decodes an audio file (wav, mp3, m4a, ...) into mono float samples at \a sampleRate.

    Channels are averaged, and the result is resampled with a low-pass filter (no aliasing above the target's
    Nyquist frequency). Decoding uses the platform's codecs (Media Foundation on Windows).
 */
bool loadAudioFile(const std::string &path, int sampleRate, std::vector<float> *samples, std::string *error);

/*! \brief Resamples \a input with a windowed-sinc low-pass filter. */
std::vector<float> resample(const std::vector<float> &input, int inputRate, int outputRate);

/*! \brief Scales \a samples so that the loudest one is at \a peak (does nothing for silence). */
void normalize(std::vector<float> *samples, float peak);
