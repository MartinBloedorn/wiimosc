# Sensor fusion

`wiimosc-stream --fusion madg` turns a Wiimote Plus's MotionPlus gyro and accelerometer into a full orientation: roll, pitch **and yaw** (`mote/rpy`), and a quaternion (`mote/quat`). This document collects the design, the decisions, and the hardware findings behind them, most of which were measured on real remotes. The OSC side (options, addresses, behaviour for receivers) is in [Streaming.md](Streaming.md#sensor-fusion---fusion); the handover summary is in [DevelopmentNotes.md](DevelopmentNotes.md) §6c.

Last updated: 2026-10-18.

## 1. Goal & status

The user's goal: full Euler angles including yaw at all times, drift accepted. The IR camera is planned as an absolute yaw reference later, likely with an EKF (§10).

| Item | State |
|---|---|
| `--fusion madg`: calibration read, startup, `rpy`, `quat`, axis signs | ✅ user-confirmed live on a Wiimote Plus ("works out of the box") |
| Gyro sensitivity correction (§5) | ✅ measured on both Wiimote Plus; yaw within ±1% per 360° turn |
| Accelerometer gate and rest gain (§6) | ✅ verified offline on recordings from both remotes; not yet tried live by the user |
| MotionPlus *accessory* (on an older Wiimote) | ❓ not measured |
| IR as yaw reference (EKF) | ❌ planned |

## 2. Overview

```
wiiuse input report (event(), WIIUSE_EVENT, exp.type == EXP_MOTION_PLUS)
  │  raw_gyro (14-bit pitch/roll/yaw), acc_mode (slow flags), gforce (g), host time
  ▼
WiimoteStreamer::updateFusion ── once per MotionPlus activation: read calibration (0x04a40020) → setCalibration
  ▼
MotionFusion::addReport
  ├─ MotionPlusCalibration::rates   raw → deg/s about body x, y, z (factory zero/scale, sign fix, sensitivity fix)
  ├─ until ready: collectRest       1 s still → FilterSetup {gyro bias, gravity, report rate, gyro range}
  └─ then: AbstractOrientationFilter::update(MotionSample {dt, gyro, accel})
         └─ MadgwickFilter: FusionBias → accelerometer gate / gain → FusionAhrsUpdateNoMagnetometer
  ▼
orientation() → mote/quat;  toEulerAngles() → mote/rpy   (sent in update(), rate-limited like other values)
```

Files (`src/fusion/`, library `motionfusion`, no wiiuse dependency):

| File | Role |
|---|---|
| `Orientation.h/.cpp` | `Vector3`, `Quaternion` (body → world), `EulerAngles`; `toEulerAngles()`, `tiltFromGravity()` |
| `MotionPlusCalibration.h/.cpp` | Parses the 32-byte calibration (CRC-checked), converts raw values to deg/s |
| `AbstractOrientationFilter.h` | Filter interface; `MotionSample` (per report) and `FilterSetup` (start state) |
| `MadgwickFilter.h/.cpp` | Fusion's AHRS plus our accelerometer handling; the only file that includes Fusion |
| `MotionFusion.h/.cpp` | Per remote: rest calibration, dt, drives the filter; `FilterType` + `parseFilterType()` |

Tools: `wiimosc-record` and `wiimosc-replay` (`src/record`, `src/replay`), with recordings in [`data/recordings/`](../data/recordings/README.md) (§8).

## 3. Algorithm choice

Without a magnetometer, the accelerometer can correct pitch and roll (gravity), but **yaw is unobservable** and drifts. Options considered:

| Option | Verdict |
|---|---|
| Pure gyro integration | Baseline only: pitch and roll drift too |
| Complementary filter | Fine, but no bias tracking or rejection logic |
| Mahony (PI complementary) | Good, ~60 lines; integral term tracks the observable bias |
| Madgwick (original IMU) | Same quality class, one gain |
| **xioTechnologies Fusion** (Madgwick's revised AHRS, C, MIT) | **Chosen** (user's pick): no-magnetometer update, `FusionBias` (bias at rest), overrange handling, maintained |
| VQF (C++, MIT) | Strong 6D filter with rest detection; heavier |
| EKF with bias states | Overkill now; the natural fit once IR provides yaw measurements |

**Build notes.** Fusion v1.3.3 is a git submodule (`external/Fusion`, unmodified), built from its `Fusion/` subfolder (library only). Its headers use C99 designated initializers and compound literals, which **MSVC only accepts in C++20**. So only the `motionfusion` target is C++20, only `MadgwickFilter.cpp` includes `Fusion.h` (behind a pimpl `State`), and no Fusion type appears in our headers. Fusion's macros like `FUSION_VECTOR_ZERO` are compound literals and don't compile in C++ at all: use `FusionVector{}`. Passing a zero accelerometer vector to `FusionAhrsUpdateNoMagnetometer` makes it integrate the gyro alone (it checks `FusionVectorIsZero`).

## 4. Sensor data & conventions

**Raw data** (wiiuse `motion_plus_t`, decoded in `motion_plus_event`): `raw_gyro.pitch/roll/yaw` are 14-bit (0–16383, zero ≈ 8000–8200 counts). `acc_mode` holds the slow-mode flags (1 = slow/precise): bit 2 = roll, bit 1 = pitch, bit 0 = yaw. Each axis switches between slow and fast mode on its own, at ~450 deg/s. In standalone mode every extension frame is a gyro frame. wiiuse's own `angle_rate_gyro` (`/20` slow, `/4` fast, plus a 0.5 deg/s dead band, first-sample zero) is unsuitable for integration and isn't used.

**Samples are taken in `WiimoteStreamer::event()` on `WIIUSE_EVENT`**, not in `update()`, which also runs after poll timeouts with no new report. wiiuse only raises `WIIUSE_EVENT` when something changed (`state_changed`), but gyro noise changes almost every report, and dt is measured from host time anyway. The remotes deliver **~180 reports/s** with the MotionPlus active. dt is clamped to 0.1 s for link stalls.

**Body frame** = the accelerometer's axes (wiiuse `gforce`, as in `mote/accel`). The gyros measure rotation about the same axes: pitch gyro → x, roll gyro → y (the long axis), yaw gyro → z. Rates are right-handed about these axes, consistent with the accelerometer. This follows Dolphin's emulation (`Source/Core/Core/HW/WiimoteEmu/MotionPlus.cpp`, `Dynamics.cpp`: "Wiimote X+ is to the left") and was **verified on hardware**: positive correlation between gyro rates and accelerometer-derived rates on x and y, and counter-clockwise table turns integrate positive on z.

**Quaternion** (`mote/quat`: w, x, y, z): rotation from body to world, world z up. Yaw 0 is the heading at the end of the rest calibration.

**Euler angles** (`toEulerAngles`): R = Rz(yaw) · Rx(pitch) · Ry(φ), i.e. yaw about the world vertical, then pitch about the body x axis, then roll about the body y axis. Gravity in the body frame is R's last row, (−cos(pitch) sin(φ), sin(pitch), cos(pitch) cos(φ)). Hence pitch = asin(g_y), which matches wiiuse's accelerometer pitch, and wiiuse's roll atan2(g_x, g_z) = −φ. So **`roll = −φ`**, keeping stock `mote/rpy` semantics for roll and pitch (checked numerically against wiiuse's formulas). Yaw is positive counter-clockwise seen from above. Gimbal lock at pitch ±90° (pointing straight up or down): yaw and roll can't be separated there, while the quaternion is fine. `tiltFromGravity()` is the inverse with yaw 0, qx(pitch) · qy(φ).

## 5. MotionPlus calibration & gyro sensitivity

**Reading.** 32 bytes at register `0x04a40020` of the *active* MotionPlus, read with `wiiuse_read_data_sync` once `exp.type == EXP_MOTION_PLUS` (a state check; the activation event can be overwritten). It's read again whenever the MotionPlus is reactivated. Layout per Dolphin: two 16-byte halves, fast then slow. Each half holds yaw, roll, pitch zero (u16 BE), yaw, roll, pitch scale (u16 BE), degrees / 6 (u8), an id byte, and half of a CRC-32 (zlib polynomial; upper 16 bits in the fast half, at 0x0e, lower in the slow half, at 0x1e) computed over bytes 0x00–0x0d and 0x10–0x1d. Values are 16-bit while the data is 14-bit, so raw is compared as `raw << 2`. If the checksum fails, Dolphin's nominal values are used (zero `0x8000`, scale offset `0x4400`, 1200°/270°).

**Conversion** (`MotionPlusCalibration::rate`): `(raw<<2 − zero) / (scale − zero) · degrees · signFix · correction`, with signFix = (−1, +1, −1) for pitch, roll, yaw (Dolphin: "M+ calibration data does not follow the right-hand rule"). The stored scale signs on both remotes are the opposite of Dolphin's emulated ones (yaw +, roll −, pitch +), and the formula still gives the right directions, so the sign comes out of the data.

**The stored sensitivity is wrong.** Read as Dolphin does, the gyro over-read by 10–30%: 360° table turns integrated to 407° / −414°, and gyro-integrated roll/pitch moved 1.30× / 1.22× as far as the accelerometer's angles. Measured slow-mode sensitivity, in 14-bit counts per deg/s:

| Axis (gyro chip) | Stored, remote 1 | Measured, remote 1 | Stored, remote 2 | Measured, remote 2 | Reference |
|---|---|---|---|---|---|
| yaw (EPSON X3500W) | 17.55 | **20.04** | 18.31 | **19.98** | 360° table turns |
| roll (InvenSense IDG-600) | 16.10 | 20.96 | 15.84 | 19.7 | accelerometer angle |
| pitch (InvenSense IDG-600) | 17.19 | 20.88 | 17.10 | 19.85 | accelerometer angle |

Remote 1 = `B8:AE:6E:56:1B:BD`, remote 2 = `8C:CD:E8:C4:D5:0F`, both `RVL-CNT-01-TR`.

The measurements don't follow the stored per-axis scales (remote 2's roll scale claims lower sensitivity than remote 1's, and its measured value is lower, but not in proportion), so a constant is the better model. Yaw agrees within 0.2% across units; roll and pitch spread ±3%. Hence `sSlowSensitivity = {20.4, 20.4, 20.0}` (pitch, roll, yaw): each axis is scaled by `storedSlowSensitivity / sSlowSensitivity`, and the same factor applies to fast mode, keeping the stored fast/slow ratio (verified below). Result: 357° / −364° (remote 1) and 359° / −360° (remote 2) per full turn; roll/pitch within ±3.5% of the accelerometer, which corrects them anyway. wiiuse's `/20` was nearly right; WiiBrew's 13.768 counts/deg/s (8192/595) is far off. Dolphin's real-Wiimote path (`InputCommon/ControllerInterface/Wiimote/WiimoteController.cpp`) uses the same uncorrected conversion, so it likely over-reads too. A pointer use case wouldn't reveal that.

**Fast mode.** Across 360 slow/fast switches in the recordings, the rate before and after a switch agreed (scale 0.97–1.01, mean offset ±3 deg/s, which is extrapolation noise at high angular accelerations). The fast-mode measurement range is about ±1700 deg/s after the correction (`range()`, passed to Fusion as the gyro range). The roll gyro was seen **saturating** (raw 16383) at ~1770 deg/s in hard shakes; rotation lost there can't be recovered.

## 6. Startup and filter

**Rest calibration** (`MotionFusion::collectRest`): the remote must be still for **1 s**: each gyro axis may vary by at most 4 deg/s and the acceleration magnitude by at most 0.1 g (`sRestGyroSpread`, `sRestAccelSpread`). Any movement restarts the window. The window's means give the gyro bias and the gravity vector, and its count the report rate. (A constant slow rotation can't be told from bias.) Nothing is sent on `mote/rpy`/`mote/quat` until then, so receivers never mix wiiuse's angles with fused ones. The streamer prints `fusion: ready (gyro bias ...; N reports/s)`.

**Filter start** (`MadgwickFilter::start`): Fusion is configured with the *measured* report rate (Fusion counts timeouts and ramps in samples), the gyro range, and its acceleration rejection **off**. Its orientation is set to `tiltFromGravity(gravity)` (yaw 0) and its 3 s startup ramp is skipped, so the output is valid immediately. `FusionBias` is seeded with the measured bias, and keeps re-estimating it whenever the remote is still (< 3 deg/s for 3 s, high-pass at 0.02 Hz). Each update gets the measured dt.

**Accelerometer handling** (`MadgwickFilter::update`, ours instead of Fusion's rejection):

- The accelerometer is used only while its magnitude is within **20%** of the one measured at rest (`sAccelTolerance`); otherwise a zero vector is passed, i.e. gyro only. The reference is the measured rest magnitude, since it differs per remote (1.05 g vs. 0.99 g).
- Gain **1.0 while moving**, **5.0 while still** (|ω| < 10 deg/s after bias, and magnitude within 3%). The gain switches via `FusionAhrsSetSettings` when the state changes. At rest the accelerometer is trustworthy, so the tilt returns to it within ~1 s after motion.

**Why not Fusion's acceleration rejection.** It judges the accelerometer by its disagreement with the current *estimate* (10° threshold, 5 s recovery timeout). On hardware this failed both ways. During ~13 s of continuous shaking it first ignored the accelerometer (correct), then its recovery kicked in and used *every* sample while the remote was still being shaken at up to 6.9 g, which dragged pitch and roll off. And once the estimate was more than 10° off, it rejected valid samples at rest until the timeout, so a tilted rest stayed > 10° off for seconds. A gate on the measurement itself can't lock itself out like that.

**Overrange**: beyond 98% of the gyro range, Fusion soft-restarts. That keeps the quaternion but ramps the gain from 10 down over 3 s, so it briefly trusts the accelerometer more, which is right after the gyro saturated.

## 7. Hardware findings

| Finding | Value |
|---|---|
| Report rate with MotionPlus | ~178–185 reports/s |
| Gyro bias at rest, remote 1 | −4.2 / −11.4 / −10.4 deg/s (x/y/z); about an hour later, after reconnecting: −3.6 / −9.0 / −8.9 |
| Gyro bias at rest, remote 2 | −4.3 / −14.0 / −12.1 deg/s |
| Bias stability within a session | ~0.02 deg/s over 4 min, no warm-up drift; it changes between connections, which the startup measurement covers |
| Gyro noise at rest | sd ~0.1 deg/s (slow mode) |
| Accelerometer resolution | 8-bit: 0.04 g (≈ 2.2°) steps. With `WITH_ACCEL_10BIT` (now on): x ~0.01 g, y/z ~0.02 g |
| Accelerometer magnitude | remote 1: 1.05 g at rest, 1.0–1.08 g across quasi-static tilts of ±75° (axes consistent); remote 2: 0.99 g |
| Accelerometer vs. gyro timing | no lag (best alignment at 0–25 ms) |
| Fast mode | from ~450 deg/s; consistent with slow mode within ~1% |
| Saturation | roll gyro at ~1770 deg/s (raw 16383) in hard shakes |
| Rumble | disturbs both sensors for ~0.3 s (gyro spread ~60 counts, \|a\| 0.6–1.4 g) |

## 8. Tuning method & tools

The tuning was done on **recordings**: raw data from real remotes, replayed offline through the pipeline and through filter variants. This was much faster and more conclusive than live testing, and it should be repeated for the EKF.

- **`wiimosc-record <file.csv> [--seconds N] [--cues 6,12,...] [--address <addr>]`** logs every MotionPlus report (time, raw gyro, slow flags, gforce) and the calibration block. With `--cues`, two short buzzes start step 0, one short buzz starts each further step, and a long buzz ends it. Format and the content of each recording: [`data/recordings/README.md`](../data/recordings/README.md).
- **`wiimosc-replay <file.csv> [--every s]`** runs the real `MotionFusion` and prints t, roll, pitch, yaw, quaternion, the tilt error against the accelerometer, and |a| as CSV.

**Scripted motions** (the person follows the buzzes): rest; one full 360° turn each way, sliding on the table and lining up with its edge; slow roll ±45–70°; slow pitch ±45–60°; vigorous motion, then put down flat; vigorous motion, then put down tilted.

**Checks that worked** (written as one-off analysis programs during the session, not kept in the repo; each is a few dozen lines on top of the replay code):

| Check | How | Measures |
|---|---|---|
| Yaw scale | Integrate the bias-corrected z rate across a 360° table turn | yaw sensitivity, sign |
| Roll/pitch scale and lag | 1D-integrate −ω_y (roll) or ω_x (pitch) over a slow phase; least-squares fit of the accelerometer angle (atan2(g_x, g_z), resp. asin-type pitch) to it, for time shifts of −200…+300 ms | sensitivity (slope), lag (best shift) |
| Signs / axis mapping | Correlate gyro rates with accelerometer-derived rates ω⊥ = −(g × ġ)/\|g\|² on 50 ms averages | signs, permutation |
| Fast-mode scale | At each slow/fast switch, compare the rate extrapolated from the two samples before with the one after | fast/slow consistency |
| Accelerometer consistency | \|a\| of quasi-static samples (\|ω\| < 15 deg/s) per tilt bucket | per-axis accelerometer scale |
| Settling | Tilt error against the accelerometer (0.2 s averages) at 0.5, 1, 2 s after a put-down | gate/gain behaviour |
| Fusion internals | `FusionAhrsGetInternalStates` / `GetFlags` per second: accelerometer ignored %, recovery % | why it misbehaved |

**Variant comparison** (recording 2, after the scale fix, then still a single 20.5 counts/deg/s on all axes; tilt error against the accelerometer in degrees after a put-down, the remote at rest):

| Variant | flat +0 s | +0.5 s | +1.2 s | tilted +0 s | +0.5 s | +1.3 s |
|---|---|---|---|---|---|---|
| Fusion: gain 0.5, rejection 10°/5 s (initial version) | 3.7 | 2.7 | 1.8 | 23.1 | 17.8 | 12.4 |
| Gate 10%, gain 0.5 | 16.7 | 10.3 | 7.2 | 32.7 | 25.7 | 17.6 |
| Gate 10%, gain 1.0, rest 3.0 | 16.2 | 5.2 | 1.0 | 18.6 | 7.8 | 1.4 |
| Gate 10%, gain 1.0, rest 5.0 | 13.3 | 2.4 | 0.3 | 17.7 | 5.2 | 0.8 |
| **Gate 20%, gain 1.0, rest 5.0** (chosen) | 8.0 | 1.7 | 0.3 | 13.7 | 4.0 | 0.7 |
| Fusion rejection + rest 5.0 | 3.9 | 1.3 | 0.3 | 20.6 | 7.1 | 0.9 |

Before the scale fix, every variant showed ~8° mean disagreement during *slow* motion; after it, 1.7–3.9°. The scale error was the main cause of the drift the user saw, the accelerometer handling the second. The error at the moment of a put-down comes from the vigorous motion before it, where the accelerometer is unusable and the gyro saturates. A wider gate keeps more accelerometer correction during motion. Rest jitter with gain 5: sd 0.01–0.04° (no visible noise).

**Pitfalls**: the hub's connection rumble was mistaken for the first cue (the first recording's steps are one phase late; hence the double-buzz start). Cue buzzes disturb the data, so leave ~0.3 s after each. Averaging one signal and not the other fakes a lag. A recording has to *end* with a rest to measure settling.

## 9. Earlier verification

- **Simulation** (before hardware): a simulated remote with bias 1.5 / −2.5 / 3 deg/s and raw values encoded with the nominal calibration (yaw 90°, pitch 45°, roll 30°, a 600 deg/s spin, 80 s rest) tracked within ~1°, with the bias estimated within 0.03 deg/s. This verified internal consistency, i.e. Euler/tilt round trips, signs within the pipeline, and fast mode, but not the hardware assumptions.
- **CRC**: our CRC-32 matches the standard check value (`"123456789"` → `CBF43926`), and a corrupted block is rejected.
- **Rest, 4 min** (remote 1, 8-bit): fused roll/pitch equal to the accelerometer within 0.03°, yaw 0.25° in 4 min.

## 10. Open work

- **IR as yaw reference** (EKF): add a class implementing `AbstractOrientationFilter`, a `FilterType` value and its name in `parseFilterType`, and IR measurements in `MotionSample` (filters use what they support). An EKF can estimate the gyro bias (and per-axis scale) as states, starting from `FilterSetup::gyroBias`. Compare it against Madgwick with `wiimosc-replay` on `data/recordings/`. That needs recordings with IR data, so extend `wiimosc-record`. wiiuse's own IR yaw (`calc_yaw`, written to `orient.yaw`) is ignored when fusion is on.
- New motion recordings with `WITH_ACCEL_10BIT` (the motion recordings so far are 8-bit), to re-check the gate and gains with the finer accelerometer.
- A MotionPlus accessory on an older Wiimote: measure its sensitivity (one 360° table turn).
- Re-zeroing yaw (a button or an OSC message).
- Nunchuk pass-through: needs the Nunchuk calibration read first ([DevelopmentNotes](DevelopmentNotes.md) §5), and halves the gyro rate.
- Very slow, steady turns (< 3 deg/s for several seconds) can be absorbed into the bias by `FusionBias`.
