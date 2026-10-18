#pragma once

#include "Orientation.h"

/*! \brief One input report's measurements, in the remote's body frame.

    Further measurements (the IR camera's sources, for an absolute yaw reference) belong here too: each filter uses
    the ones it supports.
 */
struct MotionSample {
    float dt = 0.0f;  //!< seconds since the previous sample
    Vector3 gyro;     //!< deg/s, from the factory calibration; still includes the gyro's bias
    Vector3 accel;    //!< g
};

/*! \brief What a filter starts from: measured by MotionFusion while the remote is still, and the gyro's range. */
struct FilterSetup {
    Vector3 gyroBias;          //!< deg/s
    Vector3 gravity;           //!< g, as measured at rest
    float sampleRate = 100.0f; //!< reports per second
    float gyroRange = 0.0f;    //!< deg/s; beyond it, the gyro saturates
};

/*! \brief Estimates the remote's orientation from its motion sensors. */
class AbstractOrientationFilter
{
public:
    virtual ~AbstractOrientationFilter() = default;

    /*! \brief (Re)starts the filter from \a setup, with yaw 0. */
    virtual void start(const FilterSetup &setup) = 0;
    virtual void update(const MotionSample &sample) = 0;
    virtual Quaternion orientation() const = 0;
};
