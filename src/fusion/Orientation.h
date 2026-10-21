#pragma once

/*! \brief A vector in the remote's body frame (the accelerometer's axes, as in mote/accel), or in the world frame. */
struct Vector3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

/*! \brief Rotation from the remote's body frame to the world frame (z up, yaw 0 = heading at calibration). */
struct Quaternion {
    float w = 1.0f;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

/*! \brief Angles in degrees, with the same meaning and signs as wiiuse's accelerometer roll and pitch.

    Rotation order: yaw about the world's vertical, then pitch about the remote's x axis (pointing up/down), then
    roll about its y axis (its long axis). Yaw and roll are undefined while pointing straight up or down.
 */
struct EulerAngles {
    float roll = 0.0f;
    float pitch = 0.0f;
    float yaw = 0.0f;
};

EulerAngles toEulerAngles(const Quaternion &q);

/*! \brief The orientation (with yaw 0) of a remote at rest that measures \a gravity (in g, any length). */
Quaternion tiltFromGravity(const Vector3 &gravity);

/*! \brief The rotation \a a, then \a b: a * b applied to a vector rotates it by b first. */
Quaternion multiply(const Quaternion &a, const Quaternion &b);

/*! \brief \a v rotated by \a q (e.g. from the body frame to the world frame). */
Vector3 rotate(const Quaternion &q, const Vector3 &v);

/*! \brief A rotation about the world's vertical by \a degrees, i.e. one that adds that much yaw. */
Quaternion yawRotation(float degrees);

/*! \brief \a degrees wrapped to (-180, 180]. */
float wrapDegrees(float degrees);
