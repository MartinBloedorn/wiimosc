#pragma once

#include "Orientation.h"

#include <vector>

/*! \brief An IR emitter at a known direction, seen from where the remote is used.

    The first anchor is the home anchor: a pair of sources side by side (a sensor bar), which the camera can tell from
    any other source. All others are single sources, which are told apart by direction only.
 */
struct IrAnchor {
    float yaw = 0.0f;          //!< degrees, as mote/rpy's yaw (positive to the left)
    float elevation = 0.0f;    //!< degrees above the horizon, as mote/rpy's pitch; only used if \c hasElevation
    bool hasElevation = false;
};

/*! \brief Parses a list like "0,-60:15,60": yaw[:elevation] per anchor, the home anchor first. */
bool parseIrAnchors(const char *text, std::vector<IrAnchor> *anchors);

/*! \brief The IR sources the camera sees in one report, in its raw coordinates (wiiuse's rx/ry, 0-1023 and 0-767). */
struct IrFrame {
    struct Dot {
        float x = 0.0f;
        float y = 0.0f;
    };
    int count = 0; //!< visible sources, in dots[0..count)
    Dot dots[4];
};

/*! \brief Corrects the yaw of an orientation filter with IR anchors.

    It computes changes to a yaw offset that MotionFusion applies to the filter's orientation. Seeing the home anchor
    sets it (and resets it if it's far off); after that, every anchor in view nudges it towards that anchor's yaw. A
    single source is taken for the anchor nearest to it, if it's near enough; until the home anchor was seen, single
    sources aren't used.

    Assumes that the remote stays at about the same place: the anchors' directions are only valid from there.
 */
class IrAnchorCorrection
{
public:
    explicit IrAnchorCorrection(std::vector<IrAnchor> anchors);

    /*! \brief Forgets the sync, e.g. when the filter restarted. */
    void restart();

    /*! \brief Adds one report, and returns the change to the yaw offset (deg).

        \a corrected is the orientation with the current offset, \a rotationRate how fast it turns (deg/s). Without
        \a isCorrecting, it only finds the anchor in view, and returns 0.
     */
    float update(float dt, const Quaternion &corrected, float rotationRate, const IrFrame &frame,
                 bool isCorrecting = true);

    /*! \brief Index of the anchor in view in the last report, or -1. */
    int anchorInView() const { return mAnchorInView; }
    /*! \brief Whether the home anchor was seen since the start, so the yaw refers to the anchors. */
    bool isSynced() const { return mIsSynced; }

private:
    float nudge(float dt, float residual, float rotationRate) const;

    std::vector<IrAnchor> mAnchors;
    bool mIsSynced = false;
    int mPairReports = 0; //!< consecutive reports with a far-off home anchor in view
    int mAnchorInView = -1;
};
