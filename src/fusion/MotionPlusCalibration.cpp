#include "MotionPlusCalibration.h"

#include <algorithm>
#include <cmath>

namespace {

/*! \brief The calibration's scales don't follow the right-hand rule on pitch and yaw (per Dolphin). */
const float sSignFix[3] = {-1.0f, 1.0f, -1.0f};

/*! \brief The slow mode's actual sensitivity per axis (pitch, roll, yaw), in raw 14-bit counts per deg/s.

    Taken at face value, the stored scales give 15.8-18.3 counts per deg/s, which made the gyro over-read by 10-30%.
    Measured on two Wiimote Plus (against 360 deg table turns for yaw, and the accelerometer for roll and pitch):
    yaw 20.04 and 19.98, roll 20.96 and 19.7, pitch 20.88 and 19.85, unrelated to the stored values. Yaw has its own
    gyro chip, roll and pitch share one. The stored fast/slow ratio and signs are right.
 */
const float sSlowSensitivity[3] = {20.4f, 20.4f, 20.0f};

uint16_t bigEndian16(const uint8_t *data)
{
    return static_cast<uint16_t>((data[0] << 8) | data[1]);
}

uint32_t crc32(uint32_t crc, const uint8_t *data, unsigned size)
{
    crc = ~crc;
    for (unsigned i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

} // namespace

MotionPlusCalibration::MotionPlusCalibration()
{
    const uint16_t zero = 0x8000;
    const uint16_t offset = 0x4400;
    for (Block *block : {&mFast, &mSlow}) {
        for (int axis = 0; axis < 3; ++axis) {
            block->zero[axis] = zero;
            block->scale[axis] = sSignFix[axis] < 0.0f ? zero - offset : zero + offset;
        }
    }
    mFast.degrees = 1200.0f;
    mSlow.degrees = 270.0f;
    updateCorrection();
}

bool MotionPlusCalibration::parse(const uint8_t *data)
{
    // Two 16-byte halves (fast, slow): yaw, roll, pitch zero; yaw, roll, pitch scale; degrees / 6; an id byte; and
    // half of a CRC-32 (big-endian, upper half in the fast block) over the other 14 bytes of both halves.
    uint32_t crc = crc32(0, data, 14);
    crc = crc32(crc, data + 16, 14);
    const uint32_t stored = (static_cast<uint32_t>(bigEndian16(data + 14)) << 16) | bigEndian16(data + 30);
    if (crc != stored)
        return false;

    Block blocks[2];
    for (int i = 0; i < 2; ++i) {
        const uint8_t *half = data + 16 * i;
        Block &block = blocks[i];
        for (int axis = 0; axis < 3; ++axis) {
            // Stored yaw first, pitch last; ours are ordered by body axis.
            block.zero[axis] = bigEndian16(half + 2 * (2 - axis));
            block.scale[axis] = bigEndian16(half + 6 + 2 * (2 - axis));
            if (block.scale[axis] == block.zero[axis])
                return false;
        }
        block.degrees = 6.0f * half[12];
        if (block.degrees <= 0.0f)
            return false;
    }
    mFast = blocks[0];
    mSlow = blocks[1];
    updateCorrection();
    return true;
}

Vector3 MotionPlusCalibration::rates(const uint16_t raw[3], const bool isSlow[3]) const
{
    return Vector3{rate(0, raw[0], isSlow[0] ? mSlow : mFast), rate(1, raw[1], isSlow[1] ? mSlow : mFast),
                   rate(2, raw[2], isSlow[2] ? mSlow : mFast)};
}

float MotionPlusCalibration::range() const
{
    // The raw values are 14-bit, centered on their zero.
    float range = 0.0f;
    for (int axis = 0; axis < 3; ++axis) {
        const float span = std::abs(static_cast<float>(mFast.scale[axis]) - mFast.zero[axis]);
        const float axisRange = 0x8000 / span * mFast.degrees * mCorrection[axis];
        range = axis == 0 ? axisRange : std::min(range, axisRange);
    }
    return range;
}

float MotionPlusCalibration::rate(int axis, uint16_t raw, const Block &block) const
{
    const float value = static_cast<float>(raw << 2) - block.zero[axis];
    return value / (static_cast<float>(block.scale[axis]) - block.zero[axis]) * block.degrees * sSignFix[axis]
           * mCorrection[axis];
}

void MotionPlusCalibration::updateCorrection()
{
    for (int axis = 0; axis < 3; ++axis) {
        // The slow block's sensitivity, in 14-bit counts per deg/s.
        const float span = std::abs(static_cast<float>(mSlow.scale[axis]) - mSlow.zero[axis]) / 4.0f;
        mCorrection[axis] = span / mSlow.degrees / sSlowSensitivity[axis];
    }
}
