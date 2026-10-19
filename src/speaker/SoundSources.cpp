#include "SoundSources.h"

#include <algorithm>
#include <cmath>

namespace {

const double sPi = 3.14159265358979323846;
const double sTailSeconds = 0.1; //!< silence after a sound, lets the ADPCM predictor settle back to 0

} // namespace

SineSource::SineSource(double frequency, int sampleRate)
    : mIncrement(2.0 * sPi * frequency / sampleRate)
{
}

int SineSource::read(float *samples, int count)
{
    for (int i = 0; i < count; ++i) {
        samples[i] = static_cast<float>(sSoundPeak * std::sin(mPhase));
        mPhase = std::fmod(mPhase + mIncrement, 2.0 * sPi);
    }
    return count;
}

SoundPlayer::SoundPlayer(int sampleRate)
    : mSampleRate(sampleRate)
{
}

void SoundPlayer::playSine(double frequency)
{
    std::lock_guard<std::mutex> lock(mMutex);
    mClip.reset();
    mSine = std::make_unique<SineSource>(frequency, mSampleRate);
}

void SoundPlayer::playClip(std::shared_ptr<const std::vector<float>> samples)
{
    std::lock_guard<std::mutex> lock(mMutex);
    mSine.reset();
    mClip = std::move(samples);
    mPosition = 0;
}

void SoundPlayer::stop()
{
    std::lock_guard<std::mutex> lock(mMutex);
    if (mSine || mClip)
        endSound();
}

int SoundPlayer::read(float *samples, int count)
{
    std::lock_guard<std::mutex> lock(mMutex);
    if (mSine)
        return mSine->read(samples, count);

    // Full reports of silence after a clip's end, or as the tail
    int filled = 0;
    if (mClip) {
        filled = static_cast<int>(std::min<size_t>(count, mClip->size() - mPosition));
        std::copy(mClip->begin() + mPosition, mClip->begin() + mPosition + filled, samples);
        mPosition += filled;
        if (mPosition >= mClip->size())
            endSound();
        if (filled == count)
            return count;
    } else if (mTailSamples <= 0) {
        return 0;
    } else {
        mTailSamples -= count;
    }
    std::fill(samples + filled, samples + count, 0.0f);
    return count;
}

void SoundPlayer::endSound()
{
    mSine.reset();
    mClip.reset();
    mTailSamples = static_cast<int>(sTailSeconds * mSampleRate);
}
