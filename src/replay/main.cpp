/*! \brief Replay tool: runs a wiimosc-record recording through MotionFusion, and prints the orientation as CSV.

    For comparing filters or settings offline. Besides the angles and quaternion, each row has the tilt error: the
    angle between the filter's gravity and the measured acceleration (meaningful while the remote is still), and
    with --ir-anchors the anchor in view (-1: none) and the yaw offset the anchors applied.
 */

#include "fusion/MotionFusion.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

const float sRadToDeg = 57.29577951f;

/*! \brief Angle (deg) between the filter's gravity direction and the acceleration. */
float tiltError(const Quaternion &q, const Vector3 &a)
{
    // Gravity in the body frame: the rotation matrix's last row.
    const Vector3 g{2.0f * (q.x * q.z - q.w * q.y), 2.0f * (q.y * q.z + q.w * q.x),
                    1.0f - 2.0f * (q.x * q.x + q.y * q.y)};
    const float length = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
    if (length <= 0.0f)
        return 0.0f;
    const float cosine = (g.x * a.x + g.y * a.y + g.z * a.z) / length;
    return std::acos(std::clamp(cosine, -1.0f, 1.0f)) * sRadToDeg;
}

void printUsage()
{
    std::printf("usage: wiimosc-replay <file.csv> [options]\n"
                "  --fusion <name>    filter, as for wiimosc-stream (default madg)\n"
                "  --every <s>        print a row at most this often (default 0: every report)\n"
                "  --ir-anchors <yaw[:elevation],...>\n"
                "                     correct the yaw with these IR anchors, as wiimosc-stream's --ir anchor=...\n"
                "                     (needs a recording made with --ir)\n"
                "Prints t (s, from the first report), roll, pitch, yaw (deg), qw, qx, qy, qz, tilt error (deg) and\n"
                "|a| (g) once the fusion is ready, then with --ir-anchors the anchor in view and the yaw offset (deg);\n"
                "a summary goes to stderr.\n");
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 2 || argv[1][0] == '-') {
        printUsage();
        return 1;
    }
    FilterType type = FilterType::Madgwick;
    double every = 0.0;
    std::vector<IrAnchor> anchors;
    for (int i = 2; i < argc; ++i) {
        const bool hasValue = i + 1 < argc;
        if (std::strcmp(argv[i], "--fusion") == 0 && hasValue) {
            if (!parseFilterType(argv[++i], &type) || type == FilterType::None) {
                printUsage();
                return 1;
            }
        } else if (std::strcmp(argv[i], "--every") == 0 && hasValue) {
            every = std::atof(argv[++i]);
        } else if (std::strcmp(argv[i], "--ir-anchors") == 0 && hasValue) {
            if (!parseIrAnchors(argv[++i], &anchors)) {
                printUsage();
                return 1;
            }
        } else {
            printUsage();
            return 1;
        }
    }
    FILE *file = std::fopen(argv[1], "r");
    if (!file) {
        std::fprintf(stderr, "can't read %s\n", argv[1]);
        return 1;
    }

    MotionFusion fusion(type);
    fusion.setIrAnchors(anchors);
    const bool isCorrecting = !anchors.empty();
    bool hasCalibration = false;
    bool hasStart = false;
    bool hasIr = false;
    bool wasSynced = false;
    double start = 0.0;
    double nextRow = 0.0;
    int count = 0;
    char line[512];
    std::printf("t,roll,pitch,yaw,qw,qx,qy,qz,tiltError,accel%s\n", isCorrecting ? ",anchor,yawOffset" : "");
    while (std::fgets(line, sizeof(line), file)) {
        if (std::strncmp(line, "#cal", 4) == 0) {
            uint8_t data[MotionPlusCalibration::sSize];
            char *p = line + 4;
            for (uint8_t &byte : data)
                byte = static_cast<uint8_t>(std::strtoul(p, &p, 16));
            MotionPlusCalibration calibration;
            hasCalibration = calibration.parse(data);
            fusion.setCalibration(calibration);
            continue;
        }
        if (line[0] == '#')
            continue;

        double time = 0.0;
        unsigned raw[3];
        int slow[3];
        Vector3 accel;
        int irCount = 0;
        int ir[8];
        const int fields = std::sscanf(line, "%lf,%u,%u,%u,%d,%d,%d,%f,%f,%f,%d,%d,%d,%d,%d,%d,%d,%d,%d", &time,
                                       &raw[0], &raw[1], &raw[2], &slow[0], &slow[1], &slow[2], &accel.x, &accel.y,
                                       &accel.z, &irCount, &ir[0], &ir[1], &ir[2], &ir[3], &ir[4], &ir[5], &ir[6],
                                       &ir[7]);
        if (fields < 10)
            continue;
        IrFrame frame;
        const bool hasFrame = fields == 19;
        if (hasFrame) {
            hasIr = true;
            for (int i = 0; i < 4; ++i) {
                if (ir[2 * i] >= 0 && frame.count < std::min(irCount, 4))
                    frame.dots[frame.count++] = IrFrame::Dot{static_cast<float>(ir[2 * i]),
                                                             static_cast<float>(ir[2 * i + 1])};
            }
        }
        if (!hasStart) {
            hasStart = true;
            start = time;
        }
        const uint16_t rawGyro[3] = {static_cast<uint16_t>(raw[0]), static_cast<uint16_t>(raw[1]),
                                     static_cast<uint16_t>(raw[2])};
        const bool isSlow[3] = {slow[0] != 0, slow[1] != 0, slow[2] != 0};
        const bool wasReady = fusion.isReady();
        fusion.addReport(time, rawGyro, isSlow, accel, hasFrame ? &frame : nullptr);
        ++count;
        if (!fusion.isReady())
            continue;
        const double t = time - start;
        if (!wasReady) {
            const FilterSetup &setup = fusion.setup();
            std::fprintf(stderr, "calibration %s; ready at %.2f s: gyro bias %.3f, %.3f, %.3f deg/s, %.1f reports/s\n",
                         hasCalibration ? "read" : "nominal", t, setup.gyroBias.x, setup.gyroBias.y, setup.gyroBias.z,
                         setup.sampleRate);
        }
        const IrAnchorCorrection *correction = fusion.irCorrection();
        if (correction && correction->isSynced() && !wasSynced)
            std::fprintf(stderr, "synced to the home anchor at %.2f s\n", t);
        wasSynced = correction && correction->isSynced();
        if (t < nextRow)
            continue;
        nextRow = t + every;

        const Quaternion q = fusion.orientation();
        const EulerAngles angles = toEulerAngles(q);
        std::printf("%.4f,%.3f,%.3f,%.3f,%.5f,%.5f,%.5f,%.5f,%.3f,%.4f", t, angles.roll, angles.pitch, angles.yaw,
                    q.w, q.x, q.y, q.z, tiltError(q, accel),
                    std::sqrt(accel.x * accel.x + accel.y * accel.y + accel.z * accel.z));
        if (correction)
            std::printf(",%d,%.3f", correction->anchorInView(), correction->yawOffset());
        std::printf("\n");
    }
    std::fclose(file);
    if (isCorrecting && !hasIr)
        std::fprintf(stderr, "no IR data in this recording: nothing corrected\n");
    std::fprintf(stderr, "%d reports\n", count);
    return 0;
}
