#pragma once

#include "WiimoteSpeaker.h"

#include <memory>
#include <mutex>
#include <vector>

const float sSoundPeak = 0.9f; //!< level for sines and (normalized) files

/*! \brief An endless sine, at sSoundPeak. */
class SineSource : public AbstractSoundSource
{
public:
    SineSource(double frequency, int sampleRate);

    int read(float *samples, int count) override;

private:
    double mIncrement;
    double mPhase = 0.0;
};

/*! \brief Plays one sound at a time (a sine or a clip), switched from any thread; silent otherwise.

    After each sound, whether it ended or was stopped, a short silence is played, so the ADPCM predictor settles back
    to 0. Then read() returns 0, and the speaker sends nothing until the next sound.
 */
class SoundPlayer : public AbstractSoundSource
{
public:
    explicit SoundPlayer(int sampleRate);

    /*! \brief Plays a sine until stop() or the next sound. */
    void playSine(double frequency);
    /*! \brief Plays \a samples (at the player's sample rate) once, from the start. */
    void playClip(std::shared_ptr<const std::vector<float>> samples);
    void stop();

    int read(float *samples, int count) override;

private:
    void endSound();

    std::mutex mMutex;
    int mSampleRate;
    std::unique_ptr<SineSource> mSine;
    std::shared_ptr<const std::vector<float>> mClip;
    size_t mPosition = 0;
    int mTailSamples = 0; //!< silence still to play after the last sound
};
