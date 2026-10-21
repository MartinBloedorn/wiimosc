#pragma once

#include "AbstractOrientationFilter.h"
#include "IrAnchors.h"
#include "MotionPlusCalibration.h"

#include <cstdint>
#include <memory>
#include <vector>

enum class FilterType {
    None,    //!< no fusion: wiiuse's own orientation
    Madgwick //!< MadgwickFilter
};

/*! \brief Parses a --fusion argument: "madg", or "off"/"none". */
bool parseFilterType(const char *name, FilterType *type);

/*! \brief Turns a Wiimote Plus's input reports into an orientation, with one AbstractOrientationFilter.

    It first waits for the remote to lie still for a moment, to measure the gyro's bias, the gravity (which gives
    the initial pitch and roll) and the report rate. The filter then starts with yaw 0, until IR anchors (if any) or
    yaw hints (hintYaw()) correct it. Both shift a yaw offset on top of the filter's orientation.
 */
class MotionFusion
{
public:
    explicit MotionFusion(FilterType type);

    void setCalibration(const MotionPlusCalibration &calibration);
    /*! \brief Corrects the yaw with these IR anchors (see IrAnchorCorrection); none: no correction. */
    void setIrAnchors(const std::vector<IrAnchor> &anchors);
    /*! \brief Starts over: waits for the remote to be still again. */
    void restart();

    /*! \brief Adds one input report.

        \a time is in seconds, \a rawGyro and \a isSlow are ordered pitch, roll, yaw (wiiuse's raw_gyro and acc_mode
        bits), and \a accel is in g. \a ir is the camera's view, if it's on.
     */
    void addReport(double time, const uint16_t rawGyro[3], const bool isSlow[3], const Vector3 &accel,
                   const IrFrame *ir = nullptr);

    /*! \brief Moves the yaw towards \a yaw (deg, as mote/rpy's) by the share \a strength (0-1); 1 sets it.

        Hints overrule the IR anchors: they don't correct the yaw until a while after the last hint. Ignored until
        ready, and while pointing nearly straight up or down, where yaw is ill-defined. Returns whether it's applied.
     */
    bool hintYaw(float yaw, float strength);

    /*! \brief Whether the bias is measured, and orientation() valid. */
    bool isReady() const { return mIsReady; }
    const FilterSetup &setup() const { return mSetup; }
    /*! \brief The filter's orientation, with the yaw offset. */
    Quaternion orientation() const;
    /*! \brief Degrees of yaw added to the filter's, by the IR anchors and yaw hints. */
    float yawOffset() const { return mYawOffset; }
    /*! \brief The IR anchor correction, if there are anchors. */
    const IrAnchorCorrection *irCorrection() const { return mIrCorrection.get(); }

private:
    /*! \brief Collects reports while the remote is still; true once it's been still for long enough. */
    bool collectRest(double time, const Vector3 &gyro, const Vector3 &accel);

    std::unique_ptr<AbstractOrientationFilter> mFilter;
    std::unique_ptr<IrAnchorCorrection> mIrCorrection;
    MotionPlusCalibration mCalibration;
    bool mIsReady = false;
    bool mHasTime = false;
    double mLastTime = 0.0;
    FilterSetup mSetup;
    float mYawOffset = 0.0f;
    double mHintHoldEnd = 0.0; //!< report time until which the IR anchors don't correct

    // The rest window: sums and ranges of the reports since it began.
    double mWindowStart = 0.0;
    int mWindowCount = 0;
    Vector3 mGyroSum, mGyroMin, mGyroMax;
    Vector3 mAccelSum;
    float mAccelMin = 0.0f, mAccelMax = 0.0f; //!< magnitudes
};
