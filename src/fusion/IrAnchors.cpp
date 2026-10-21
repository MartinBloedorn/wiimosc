#include "IrAnchors.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>

namespace {

const float sRadToDeg = 57.29577951f;

/*! \brief The camera's horizontal field of view (deg): 39.7 measured (dot motion against the gyro's yaw). */
const float sFieldOfView = 40.0f;
const float sCameraCenterX = 511.5f;
const float sCameraCenterY = 383.5f;
/*! \brief Focal length in pixels (square pixels). */
const float sFocalLength = 512.0f / std::tan(0.5f * sFieldOfView / sRadToDeg);

/*! \brief The widest a home pair may appear (deg): a 20 cm sensor bar from 45 cm away. */
const float sMaxPairSpan = 25.0f;
/*! \brief How far a home pair may be from horizontal: vertical separation per horizontal separation. */
const float sMaxPairSlope = 0.35f;
/*! \brief A home pair this far off (deg) resets the offset, instead of nudging it. */
const float sResyncThreshold = 15.0f;
/*! \brief Reports in a row the far-off home pair must be seen, against brief false pairs. */
const int sResyncReports = 3;

/*! \brief A single source is taken for an anchor this near it (deg), and ignored otherwise. */
const float sYawGate = 20.0f;
const float sElevationGate = 15.0f;

/*! \brief The nudge's time constant (s), and its limit: a constant rate plus a share of the turning rate. */
const float sTimeConstant = 0.5f;
const float sStillCorrectionRate = 2.0f;
const float sMovingCorrectionShare = 0.2f;

/*! \brief A source's direction in the world frame, as yaw and elevation (deg). */
struct Bearing {
    Vector3 direction; //!< unit length
    float yaw = 0.0f;
    float elevation = 0.0f;
};

Bearing toBearing(const Vector3 &direction)
{
    Bearing bearing;
    bearing.direction = direction;
    // Like toEulerAngles(): yaw is the heading of the body's y axis, and pitch its elevation.
    bearing.yaw = std::atan2(-direction.x, direction.y) * sRadToDeg;
    bearing.elevation = std::asin(std::clamp(direction.z, -1.0f, 1.0f)) * sRadToDeg;
    return bearing;
}

/*! \brief Direction of a camera position, with orientation \a q.

    The camera looks along the body's y axis. Its x (wiiuse's rx) grows to the left, and its y downwards, while the
    body's x points right and its z up. Measured on a recording of sweeps and rolls across a sensor bar: only these
    signs keep the bar's direction constant (1.4 deg spread, against 10 with x the other way).
 */
Bearing bearingOf(const Quaternion &q, const IrFrame::Dot &dot)
{
    const Vector3 body{-(dot.x - sCameraCenterX) / sFocalLength, 1.0f, -(dot.y - sCameraCenterY) / sFocalLength};
    const float length = std::sqrt(body.x * body.x + body.y * body.y + body.z * body.z);
    return toBearing(rotate(q, Vector3{body.x / length, body.y / length, body.z / length}));
}

bool isElevationNear(const IrAnchor &anchor, float elevation)
{
    return !anchor.hasElevation || std::fabs(elevation - anchor.elevation) <= sElevationGate;
}

/*! \brief Whether two sources look like a sensor bar; if so, \a center is its middle. */
bool isPair(const Bearing &a, const Bearing &b, Bearing *center)
{
    const float cosine = a.direction.x * b.direction.x + a.direction.y * b.direction.y + a.direction.z * b.direction.z;
    const float span = std::acos(std::clamp(cosine, -1.0f, 1.0f)) * sRadToDeg;
    const float horizontal = std::fabs(wrapDegrees(a.yaw - b.yaw)) * std::cos(0.5f * (a.elevation + b.elevation) / sRadToDeg);
    const float vertical = std::fabs(a.elevation - b.elevation);
    if (span > sMaxPairSpan || vertical > sMaxPairSlope * horizontal)
        return false;
    const Vector3 sum{a.direction.x + b.direction.x, a.direction.y + b.direction.y, a.direction.z + b.direction.z};
    const float length = std::sqrt(sum.x * sum.x + sum.y * sum.y + sum.z * sum.z);
    *center = toBearing(Vector3{sum.x / length, sum.y / length, sum.z / length});
    return true;
}

} // namespace

bool parseIrAnchors(const char *text, std::vector<IrAnchor> *anchors)
{
    anchors->clear();
    for (const char *p = text; *p;) {
        char *end = nullptr;
        IrAnchor anchor;
        anchor.yaw = std::strtof(p, &end);
        if (end == p)
            return false;
        if (*end == ':') {
            p = end + 1;
            anchor.elevation = std::strtof(p, &end);
            if (end == p)
                return false;
            anchor.hasElevation = true;
        }
        anchor.yaw = wrapDegrees(anchor.yaw);
        anchors->push_back(anchor);
        if (*end != ',' && *end != '\0')
            return false;
        p = *end == ',' ? end + 1 : end;
    }
    return !anchors->empty();
}

IrAnchorCorrection::IrAnchorCorrection(std::vector<IrAnchor> anchors)
    : mAnchors(std::move(anchors))
{
}

void IrAnchorCorrection::restart()
{
    mIsSynced = false;
    mPairReports = 0;
    mAnchorInView = -1;
}

float IrAnchorCorrection::update(float dt, const Quaternion &corrected, float rotationRate, const IrFrame &frame,
                                 bool isCorrecting)
{
    mAnchorInView = -1;
    if (mAnchors.empty() || frame.count <= 0)
        return 0.0f;

    const int count = std::min(frame.count, 4);
    Bearing bearings[4];
    for (int i = 0; i < count; ++i)
        bearings[i] = bearingOf(corrected, frame.dots[i]);

    // The home anchor, if two sources look like it: it sets the offset, or nudges it.
    const IrAnchor &home = mAnchors[0];
    for (int i = 0; i < count; ++i) {
        for (int j = i + 1; j < count; ++j) {
            Bearing center;
            if (!isPair(bearings[i], bearings[j], &center) || !isElevationNear(home, center.elevation))
                continue;
            mAnchorInView = 0;
            const float residual = wrapDegrees(home.yaw - center.yaw);
            if (!isCorrecting) {
                mPairReports = 0;
                return 0.0f;
            }
            if (!mIsSynced || std::fabs(residual) > sResyncThreshold) {
                if (++mPairReports >= sResyncReports) {
                    mIsSynced = true;
                    mPairReports = 0;
                    return residual;
                }
                return 0.0f;
            }
            mPairReports = 0;
            return nudge(dt, residual, rotationRate);
        }
    }
    mPairReports = 0;
    if (!mIsSynced)
        return 0.0f;

    // Single sources: each is taken for the anchor nearest to it. The one that fits best is used.
    int bestAnchor = -1;
    float bestResidual = 0.0f;
    for (int i = 0; i < count; ++i) {
        for (int a = 0; a < static_cast<int>(mAnchors.size()); ++a) {
            const float residual = wrapDegrees(mAnchors[a].yaw - bearings[i].yaw);
            if (std::fabs(residual) > sYawGate || !isElevationNear(mAnchors[a], bearings[i].elevation))
                continue;
            if (bestAnchor < 0 || std::fabs(residual) < std::fabs(bestResidual)) {
                bestAnchor = a;
                bestResidual = residual;
            }
        }
    }
    if (bestAnchor < 0)
        return 0.0f;
    mAnchorInView = bestAnchor;
    // A single source near home is one of its pair (the other is out of view): off by half the pair's span.
    if (bestAnchor == 0 || !isCorrecting)
        return 0.0f;
    return nudge(dt, bestResidual, rotationRate);
}

float IrAnchorCorrection::nudge(float dt, float residual, float rotationRate) const
{
    // Exponentially towards the anchor, but not faster than a small rate, or a share of the turning: corrections
    // are hard to notice while the remote turns, and easy while it's pointed at something.
    const float maxStep = (sStillCorrectionRate + sMovingCorrectionShare * rotationRate) * dt;
    return std::clamp(residual * std::min(1.0f, dt / sTimeConstant), -maxStep, maxStep);
}
