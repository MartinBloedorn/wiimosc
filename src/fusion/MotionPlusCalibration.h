#pragma once

#include "Orientation.h"

#include <cstdint>

/*! \brief The MotionPlus's factory calibration, which converts its raw gyro values into body-frame rates.

    The MotionPlus stores two blocks, for its fast and slow (more precise) mode; each axis switches between them on
    its own. Layout and signs follow Dolphin's emulation (Source/Core/Core/HW/WiimoteEmu/MotionPlus.cpp), which
    matches real hardware: the pitch, roll and yaw gyros measure rotation about the body's x, y and z axes.

    The stored scales overstate the sensitivity, though: rates are corrected to the one measured on hardware, keeping
    the stored ratio between fast and slow mode.
 */
class MotionPlusCalibration
{
public:
    static const unsigned sAddress = 0x04a40020; //!< register address of the active MotionPlus's calibration
    static const unsigned sSize = 32;

    /*! \brief Nominal values (those of Dolphin's emulated MotionPlus), for when the real ones can't be read. */
    MotionPlusCalibration();

    /*! \brief Reads the calibration from the \c sSize bytes at \c sAddress.

        Returns false, and keeps the current values, if the checksum doesn't match.
     */
    bool parse(const uint8_t *data);

    /*! \brief Converts raw 14-bit values, ordered pitch, roll, yaw, into deg/s about the body's x, y and z axes. */
    Vector3 rates(const uint16_t raw[3], const bool isSlow[3]) const;

    /*! \brief The fast mode's measurement range, in deg/s. */
    float range() const;

private:
    struct Block {
        uint16_t zero[3];  //!< pitch, roll, yaw; 16-bit, like the scales
        uint16_t scale[3]; //!< the value measured at \c degrees deg/s
        float degrees;
    };

    float rate(int axis, uint16_t raw, const Block &block) const;
    void updateCorrection();

    Block mFast;
    Block mSlow;
    float mCorrection[3]; //!< per axis: scales the stored sensitivity to the measured one, see sSlowSensitivity
};
