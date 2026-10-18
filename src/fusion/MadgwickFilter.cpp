#include "MadgwickFilter.h"

#include <Fusion.h>

#include <algorithm>
#include <cmath>

namespace {

/*! \brief How quickly the accelerometer corrects the tilt (1/s): while moving, and while still.

    Tuned on recordings (see doc/Fusion.md): at rest the accelerometer is trustworthy, so the tilt
    returns to it within ~1 s after any motion.
 */
const float sMovingGain = 1.0f;
const float sRestGain = 5.0f;
/*! \brief The accelerometer is only used while its magnitude is within this fraction of the one at rest. */
const float sAccelTolerance = 0.2f;
/*! \brief Still: below this rate (deg/s, after bias), and with the magnitude within this fraction. */
const float sRestRate = 10.0f;
const float sRestAccelTolerance = 0.03f;

FusionVector toFusion(const Vector3 &v)
{
    FusionVector result;
    result.axis.x = v.x;
    result.axis.y = v.y;
    result.axis.z = v.z;
    return result;
}

float length(const FusionVector &v)
{
    return std::sqrt(v.axis.x * v.axis.x + v.axis.y * v.axis.y + v.axis.z * v.axis.z);
}

} // namespace

struct MadgwickFilter::State {
    FusionAhrs ahrs;
    FusionBias bias;
    FusionAhrsSettings settings;
    float restGravity = 1.0f; //!< g, the accelerometer's magnitude at rest
    bool isAtRest = false;
};

MadgwickFilter::MadgwickFilter()
    : mState(std::make_unique<State>())
{
    FusionAhrsInitialise(&mState->ahrs);
    FusionBiasInitialise(&mState->bias);
}

MadgwickFilter::~MadgwickFilter() = default;

void MadgwickFilter::start(const FilterSetup &setup)
{
    // Fusion's own acceleration rejection judges the accelerometer by how much it disagrees with the current
    // estimate. On hardware, that went wrong both ways: after 5 s of motion, its recovery used every sample of a
    // shaking remote, and once the estimate was off, it rejected valid ones at rest. The gate in update() looks at
    // the measurement only.
    FusionAhrsSettings &settings = mState->settings;
    settings = fusionAhrsDefaultSettings;
    settings.sampleRate = setup.sampleRate; // Fusion counts in samples
    settings.gain = sMovingGain;
    settings.gyroscopeRange = setup.gyroRange;
    settings.accelerationRejection = 0.0f;
    settings.rejectionTimeout = 0.0f;
    FusionAhrsInitialise(&mState->ahrs);
    FusionAhrsSetSettings(&mState->ahrs, &settings);
    mState->isAtRest = false;

    const Vector3 &g = setup.gravity;
    mState->restGravity = std::sqrt(g.x * g.x + g.y * g.y + g.z * g.z);

    FusionBiasSettings biasSettings = fusionBiasDefaultSettings;
    biasSettings.sampleRate = setup.sampleRate;
    FusionBiasInitialise(&mState->bias);
    FusionBiasSetSettings(&mState->bias, &biasSettings);
    FusionBiasSetOffset(&mState->bias, toFusion(setup.gyroBias));

    // Fusion's startup phase would converge from the identity over 3 s; the measured gravity gives the tilt at once.
    const Quaternion tilt = tiltFromGravity(setup.gravity);
    FusionQuaternion quaternion;
    quaternion.element.w = tilt.w;
    quaternion.element.x = tilt.x;
    quaternion.element.y = tilt.y;
    quaternion.element.z = tilt.z;
    FusionAhrsSetQuaternion(&mState->ahrs, quaternion);
    FusionAhrsSkipStartup(&mState->ahrs);
}

void MadgwickFilter::update(const MotionSample &sample)
{
    State &state = *mState;
    const FusionVector gyro = FusionBiasUpdate(&state.bias, toFusion(sample.gyro));
    const FusionVector accel = toFusion(sample.accel);
    const float deviation = std::abs(length(accel) / state.restGravity - 1.0f);

    const bool isAtRest = length(gyro) < sRestRate && deviation < sRestAccelTolerance;
    if (isAtRest != state.isAtRest) {
        state.isAtRest = isAtRest;
        state.settings.gain = isAtRest ? sRestGain : sMovingGain;
        FusionAhrsSetSettings(&state.ahrs, &state.settings);
    }

    // A zero accelerometer makes Fusion integrate the gyro alone.
    FusionAhrsSetSamplePeriod(&state.ahrs, sample.dt);
    FusionAhrsUpdateNoMagnetometer(&state.ahrs, gyro, deviation < sAccelTolerance ? accel : FusionVector{});
}

Quaternion MadgwickFilter::orientation() const
{
    const FusionQuaternion q = FusionAhrsGetQuaternion(&mState->ahrs);
    return Quaternion{q.element.w, q.element.x, q.element.y, q.element.z};
}
