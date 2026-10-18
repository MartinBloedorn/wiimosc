# Motion recordings

Raw MotionPlus and accelerometer data from real remotes, recorded with `wiimosc-record`, for tuning and comparing the sensor fusion offline (`wiimosc-replay`). See [doc/Fusion.md](../../doc/Fusion.md) for what was learned from them (§5–§8) and how they were analyzed (§8).

## Format

CSV with `#` comment lines:

- `# t,rawPitch,rawRoll,rawYaw,slowPitch,slowRoll,slowYaw,ax,ay,az`: the column header. Recordings made with `--ir` add `,irCount,ir0x,ir0y,ir1x,ir1y,ir2x,ir2y,ir3x,ir3y`.
- `#cal <32 hex bytes>`: the MotionPlus calibration (register `0x04a40020`), as `MotionPlusCalibration::parse()` reads it.
- `#phase <n> <t>`: the start of cued step *n* (`--cues`), on the same clock as the rows.
- Rows: `t` in seconds (steady clock, arbitrary origin); wiiuse's raw 14-bit gyro values (`raw_gyro`) and slow-mode flags (1 = slow), ordered pitch, roll, yaw (rotation about the remote's x, y, z); wiiuse's calibrated acceleration (`gforce`) in g. With `--ir`: the number of visible IR sources, then four x,y pairs in camera pixels (wiiuse's `rx`/`ry`, 0–1023 and 0–767), the visible sources first and the rest `-1,-1`.

One row per input report that wiiuse flagged as a change (~180 per second).

## Recording

```
build\Release\wiimosc-record data\recordings\<name>.csv --seconds 60
build\Release\wiimosc-record data\recordings\<name>.csv --cues 6,12,12,15,15 --address 8C:CD:E8:C4:D5:0F
```

`--address` picks the remote when several are connected (otherwise the first Wiimote Plus is recorded). `--ir` also records the IR camera (sensitivity 3); replay such recordings with `--ir-anchors <list>` to test the IR anchor correction (Fusion.md §10).

With `--cues`, two short buzzes start step 0, one short buzz starts each further step, and a long buzz ends the recording. The hub's connection rumble comes first: make clear to whoever holds the remote that it's not a cue. Each buzz disturbs both sensors for ~0.3 s.

## Replaying

```
build\Release\wiimosc-replay data\recordings\<name>.csv --every 0.5 > out.csv
```

Prints the orientation per report (or every `--every` seconds): t (from the first report), roll, pitch, yaw, the quaternion, and the tilt error against the accelerometer (meaningful while the remote is still). A summary (calibration, bias, report rate) goes to stderr.

## Recordings

All from 2026-10-18, from the Wiimote Plus `B8:AE:6E:56:1B:BD` unless noted. Times are seconds from the first row.

| File | Accelerometer | Content |
|---|---|---|
| `2026-10-18-rest-4min-8bit.csv` | 8-bit | 240 s flat and still on a table (a slight bump at ~220 s). Bias stability, noise, drift at rest. |
| `2026-10-18-motion1-8bit.csv` | 8-bit | Cued, but the person was one step behind the cues (the `#phase` lines are off by one step): rest 0–20, slow roll ±70° 20–32, slow pitch ±50° 32–44, turns on the table (roughly ±90°) 44–56, vigorous free motion 56–78 (no rest at the end). |
| `2026-10-18-motion2-8bit.csv` | 8-bit | Cued, phases match: rest 0–8, one full 360° turn counter-clockwise on the table ~9–12 then rest, one full turn clockwise ~21–25 then rest, vigorous motion 33–42 then flat rest 43–48, vigorous motion 50–60 then a tilted rest (~21° nose down) 61–63. Peaks: 6.9 g, ~1770 deg/s (the roll gyro saturates). |
| `2026-10-18-rest-20s-10bit.csv` | 10-bit (`WITH_ACCEL_10BIT`) | 20 s flat and still. |
| `2026-10-18-remote2-scale-10bit.csv` | 10-bit | **Second Wiimote Plus `8C:CD:E8:C4:D5:0F`.** Cued, phases match: rest 0–8.5, one 360° turn counter-clockwise on the table ~9–14 then rest, one turn clockwise ~21–26 then rest, slow roll 33–45, slow pitch 46–57, flat rest from ~60. |
| `2026-10-18-irbar-sweep-10bit.csv` | 10-bit, **with IR** (`--ir`) | Remote 2, a sensor bar (two sources) in front. Cued, phases match: flat rest pointing at the bar 2.6–10.6, slow sweeps left/right until the bar leaves the view 10.6–25.8, faster sweeps 25.8–41, pointing at the bar while rolling ±45–60° 41–51, sweeps up/down across it 51–61, put down pointing at it 61.4–70.7. Used to find the camera's axis signs and field of view (Fusion.md §10). |
