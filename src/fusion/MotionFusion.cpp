#include "MotionFusion.h"

#include "MadgwickFilter.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

/*! \brief How long the remote must be still at the start. */
const double sRestDuration = 1.0;
/*! \brief The most each gyro axis (deg/s) and the acceleration's magnitude (g) may vary while still. */
const float sRestGyroSpread = 4.0f;
const float sRestAccelSpread = 0.1f;
/*! \brief Longer gaps between reports (a stalled link) are integrated as this long, in seconds. */
const float sMaxDt = 0.1f;
/*! \brief How long (s) after a yaw hint the IR anchors stay overruled. */
const double sHintHold = 1.0;
/*! \brief Yaw hints are ignored above this pitch (deg, up or down). */
const float sMaxHintPitch = 80.0f;

float magnitude(const Vector3 &v)
{
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

} // namespace

bool parseFilterType(const char *name, FilterType *type)
{
    if (std::strcmp(name, "madg") == 0)
        *type = FilterType::Madgwick;
    else if (std::strcmp(name, "off") == 0 || std::strcmp(name, "none") == 0)
        *type = FilterType::None;
    else
        return false;
    return true;
}

MotionFusion::MotionFusion(FilterType type)
{
    if (type == FilterType::Madgwick)
        mFilter = std::make_unique<MadgwickFilter>();
    mSetup.gyroRange = mCalibration.range();
}

void MotionFusion::setCalibration(const MotionPlusCalibration &calibration)
{
    mCalibration = calibration;
    mSetup.gyroRange = calibration.range();
    restart();
}

void MotionFusion::setIrAnchors(const std::vector<IrAnchor> &anchors)
{
    if (anchors.empty())
        mIrCorrection.reset();
    else
        mIrCorrection = std::make_unique<IrAnchorCorrection>(anchors);
}

void MotionFusion::restart()
{
    mIsReady = false;
    mHasTime = false;
    mWindowCount = 0;
    mYawOffset = 0.0f;
    mHintHoldEnd = 0.0;
    if (mIrCorrection)
        mIrCorrection->restart();
}

void MotionFusion::addReport(double time, const uint16_t rawGyro[3], const bool isSlow[3], const Vector3 &accel,
                             const IrFrame *ir)
{
    if (!mFilter)
        return;

    const Vector3 gyro = mCalibration.rates(rawGyro, isSlow);
    const float dt = mHasTime ? static_cast<float>(time - mLastTime) : 0.0f;
    mHasTime = true;
    mLastTime = time;

    if (!mIsReady) {
        if (collectRest(time, gyro, accel)) {
            mFilter->start(mSetup);
            mIsReady = true;
        }
        return;
    }

    MotionSample sample;
    sample.dt = std::clamp(dt, 0.0f, sMaxDt);
    sample.gyro = gyro;
    sample.accel = accel;
    if (sample.dt <= 0.0f)
        return;
    mFilter->update(sample);

    if (mIrCorrection && ir) {
        const Vector3 rate{gyro.x - mSetup.gyroBias.x, gyro.y - mSetup.gyroBias.y, gyro.z - mSetup.gyroBias.z};
        const float change = mIrCorrection->update(sample.dt, orientation(), magnitude(rate), *ir,
                                                   time >= mHintHoldEnd);
        mYawOffset = wrapDegrees(mYawOffset + change);
    }
}

bool MotionFusion::hintYaw(float yaw, float strength)
{
    if (!mIsReady)
        return false;
    const EulerAngles angles = toEulerAngles(orientation());
    if (std::fabs(angles.pitch) > sMaxHintPitch)
        return false;
    mYawOffset = wrapDegrees(mYawOffset + std::clamp(strength, 0.0f, 1.0f) * wrapDegrees(yaw - angles.yaw));
    mHintHoldEnd = mLastTime + sHintHold;
    return true;
}

Quaternion MotionFusion::orientation() const
{
    if (!mIsReady)
        return Quaternion{};
    return mYawOffset == 0.0f ? mFilter->orientation() : multiply(yawRotation(mYawOffset), mFilter->orientation());
}

bool MotionFusion::collectRest(double time, const Vector3 &gyro, const Vector3 &accel)
{
    const float accelMagnitude = magnitude(accel);
    if (mWindowCount == 0) {
        mWindowStart = time;
        mGyroSum = mAccelSum = Vector3{};
        mGyroMin = mGyroMax = gyro;
        mAccelMin = mAccelMax = accelMagnitude;
    }
    ++mWindowCount;
    mGyroSum = Vector3{mGyroSum.x + gyro.x, mGyroSum.y + gyro.y, mGyroSum.z + gyro.z};
    mAccelSum = Vector3{mAccelSum.x + accel.x, mAccelSum.y + accel.y, mAccelSum.z + accel.z};
    mGyroMin = Vector3{std::min(mGyroMin.x, gyro.x), std::min(mGyroMin.y, gyro.y), std::min(mGyroMin.z, gyro.z)};
    mGyroMax = Vector3{std::max(mGyroMax.x, gyro.x), std::max(mGyroMax.y, gyro.y), std::max(mGyroMax.z, gyro.z)};
    mAccelMin = std::min(mAccelMin, accelMagnitude);
    mAccelMax = std::max(mAccelMax, accelMagnitude);

    // Any movement starts the window over. (A constant slow rotation can't be told apart from bias.)
    const bool isMoving = mGyroMax.x - mGyroMin.x > sRestGyroSpread || mGyroMax.y - mGyroMin.y > sRestGyroSpread
                          || mGyroMax.z - mGyroMin.z > sRestGyroSpread || mAccelMax - mAccelMin > sRestAccelSpread;
    if (isMoving) {
        mWindowCount = 0;
        return false;
    }

    const double duration = time - mWindowStart;
    if (duration < sRestDuration)
        return false;

    const float count = static_cast<float>(mWindowCount);
    mSetup.gyroBias = Vector3{mGyroSum.x / count, mGyroSum.y / count, mGyroSum.z / count};
    mSetup.gravity = Vector3{mAccelSum.x / count, mAccelSum.y / count, mAccelSum.z / count};
    mSetup.sampleRate = static_cast<float>((mWindowCount - 1) / duration);
    mWindowCount = 0;
    return true;
}
