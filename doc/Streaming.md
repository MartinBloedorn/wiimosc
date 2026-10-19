# OSC streaming

`wiimosc-stream` pairs and connects devices the same way `wiimosc-pair` does (see [Pairing](Pairing.md)), and streams their input as OSC over UDP.

```
wiimosc-stream --host 127.0.0.1 --port 4321 --prefix wiimosc [--in-port 4322] [--max-rate 60] [--gyro]
               [--fusion madg] [--tare] [--ir [anchor=0,-60,60]] [--scan-timeout 30]
```

| Option | Default | Meaning |
|---|---|---|
| `--host` | `127.0.0.1` | Target: host name, IPv4 address, or broadcast address (`255.255.255.255`, `192.168.1.255`). `0.0.0.0` is rejected, since it's a listening address. |
| `--port` | `4321` | Target UDP port |
| `--prefix` | `wiimosc` | Address prefix. Normalized: `wiimosc`, `/wiimosc/` → `/wiimosc`; may be empty. |
| `--in-port <port>` | off | Receives OSC on this UDP port, on all interfaces, to control the devices. See [Input](#input---in-port). |
| `--max-rate` | `0` | Max. rate (Hz) of continuous values per device; `0` = unlimited. Buttons and status messages are never limited. |
| `--gyro` | off | Activates the MotionPlus on Wiimote Plus remotes and streams `mote/gyro`. A Nunchuk on such a remote is then not read. |
| `--fusion <madg\|off>` | `off` | Sensor fusion on Wiimote Plus remotes: activates the MotionPlus (a Nunchuk is then not read), replaces `mote/rpy` with fused angles including yaw, and adds `mote/quat`. `madg` = Madgwick's AHRS ([xioTechnologies Fusion](https://github.com/xioTechnologies/Fusion)). `off` keeps wiiuse's own `mote/rpy`. See [Sensor fusion](#sensor-fusion---fusion). |
| `--tare` | off | Zeroes the Balance Board after connecting: averages its sensors for 1 s, keep it empty. |
| `--ir [anchor=<yaw[:elevation]>,...]` | off | Activates the Wiimotes' IR cameras, and streams the IR sources they see (`mote/ir/...`). With `--fusion`, IR anchors also correct the fused yaw: the first is a sensor bar (home), the others single IR sources, at the given yaw (and optionally elevation), in degrees as in `mote/rpy`. Without `anchor=`, there's just the home anchor, at yaw 0. `anchor=` without `--fusion` is an error. See [IR anchors](#ir-anchors---ir-anchor). |
| `--ir-sensitivity <1-5>` | `3` | IR camera sensitivity (wiiuse's presets; 5 picks up the faintest sources) |
| `--one-two`, `--no-auth` | | Pairing method, as for `wiimosc-pair` |
| `--id <id>=<address>` | | Always gives the device with this Bluetooth address this id (0–7). Repeatable; also available in `wiimosc-pair`. |
| `--shutdown-on-exit` | off | Switches off all connected devices when quitting (receivers get `status none` for each). See [Pairing](Pairing.md#shutting-devices-down). |
| `--idle-timeout <minutes>` | `0` (never) | Switches off devices that have been idle this long: no button changes, no motion, no Nunchuk stick movement, and an empty, unchanged Balance Board. Receivers get `status none`. Fractions work (`0.5`). |
| `--scan-timeout <seconds>` | `0` (never) | Stops scanning for remotes to pair after this long, which frees the radio for the connected devices, e.g. for the speaker. A running scan cycle finishes first (up to ~10 s more on Windows), see [Pairing](Pairing.md#scan-timeout). Devices paired before still reconnect. |

## Addresses

`<p>` is `<prefix>/<id>`, e.g. `/wiimosc/0`. `<id>` is 0-based (0–7), the same number the terminal output shows (`[0] Wiimote Plus B8:AE:6E:56:1B:BD connected`). Ids 0–3 light player LEDs 1–4.

### Fixed ids

By default, a device gets the lowest free id, so ids depend on connection order. To pin them, pass `--id <id>=<address>` per device:

```
wiimosc-stream --id 0=B8:AE:6E:56:1B:BD --id 1=8C:CD:E8:C4:D5:0F --id 2=00:1F:C5:9E:80:70
```

- On shutdown (Ctrl+C), both tools list the connected devices, and print a ready-to-paste line: `to keep these ids: --id 0=... --id 1=...`. Run once, arrange the ids as wanted, and paste.
- Addresses are case-insensitive, and may use `:`, `-`, or no separators.
- Ids given with `--id` stay reserved while their device is away: other devices take the lowest id that is neither in use nor reserved (and only fall back to reserved ids when everything else is taken).
- The address comes from the HID serial number, which Windows and macOS set to the Bluetooth address for Bluetooth HID devices. A device whose address can't be read is shown as `(unknown address)` and always gets a free id.

| Address | Arguments | Sent |
|---|---|---|
| `<p>/status` | string `mote` / `fit` / `none` | on connect / disconnect (including shutdowns by `--idle-timeout` and `--shutdown-on-exit`) |
| `<p>/battery` | float 0–1 | on connect, then every 10 s |
| `<p>/mote/button/{a,b,1,2,plus,minus,home,up,down,left,right}` | int 0/1 | on change |
| `<p>/mote/rpy` | 3 floats, degrees: roll, pitch, yaw | continuous. With `--fusion` on a Wiimote Plus: fused, and only once the fusion is ready |
| `<p>/mote/quat` | 4 floats: w, x, y, z | only with `--fusion` on a Wiimote Plus; continuous once ready: orientation, body → world (z up). Free of gimbal lock, and corrected by the IR anchors like `mote/rpy` (both come from the same quaternion) |
| `<p>/mote/accel` | 3 floats, g: x, y, z | continuous. Resolution ~0.01 g on x, ~0.02 g on y and z (the accelerometer's full 10/9/9 bits) |
| `<p>/mote/gyro` | 3 floats, deg/s: roll, pitch, yaw rates | continuous, only with `--gyro` on a Wiimote Plus |
| `<p>/mote/ir/count` | int 0–4 | only with `--ir`; on change: number of IR sources the camera sees |
| `<p>/mote/ir/xyz` | 3 floats: x, y −1–1, z 0–1 | only with `--ir`; continuous while `count` > 0: pointer position computed by wiiuse (x right, y up), and wiiuse's distance measure z (larger: farther; 0 until two sources were seen) |
| `<p>/mote/ir/anchor` | int −1, 0, 1, ... | only with `--ir` and `--fusion` on a Wiimote Plus, once the fusion is ready; on change: the IR anchor in view (its index in `anchor=`, 0 = home), −1 for none |
| `<p>/mote/ir/dot/{0,1,2,3}` | 2 floats, −1–1 | only with `--ir`; continuous while that source is visible: its raw camera position |
| `<p>/chuck/status` | int 0/1 | on connect, and when a Nunchuk is plugged in or out |
| `<p>/chuck/button/{c,z}` | int 0/1 | on change, while a Nunchuk is plugged in |
| `<p>/chuck/xy` | 2 floats, −1–1 | continuous, stick position |
| `<p>/chuck/rpy`, `<p>/chuck/accel` | 3 floats | continuous, as for the Wiimote |
| `<p>/fit/button` | int 0/1 | on change |
| `<p>/fit/weight` | float, kg | continuous, sum of the four sensors |
| `<p>/fit/raw/{0,1,2,3}` | float, kg | continuous; top left, top right, bottom left, bottom right |
| `<p>/fit/corner/{0,1,2,3}` | float, 0–1 | continuous; each sensor's share of the total weight (same order as `raw`); all `0` below 2 kg |
| `<p>/fit/xy` | 2 floats, −1–1 | continuous, center of mass (x: left → right, y: bottom → top); `0, 0` below 2 kg |

"Continuous" values are sent whenever they change, at most `--max-rate` times per second. After connecting, every value is sent once, so receivers start with a complete state.

## Input (`--in-port`)

With `--in-port <port>`, the streamer also receives OSC on that UDP port, and takes these addresses (same `<p>` as above):

| Address | Arguments | Effect |
|---|---|---|
| `<p>/mote/rumble/trigger` | number, 0–1 | Rumble motor: on above 0.5, off otherwise, until the next rumble request. Any numeric type works (float, int, double, `T`/`F`). |
| `<p>/mote/rumble/pulse` | 2 numbers: 0–1, ms | Rumble for the given duration (capped at 3000 ms), then off; the streamer switches it off. An intensity of 0.5 or less, or a duration of 0, switches the motor off right away. |
| `<p>/mote/speaker/sine` | number, Hz (up to 3000) | Plays a sine on the remote's speaker until `stop` or the next sound. |
| `<p>/mote/speaker/file` | string, path | Plays an audio file once, from the start (wav, mp3, m4a/AAC, aiff; wma on Windows). The path is on the machine running the streamer. |
| `<p>/mote/speaker/stop` | none | Stops the current sound. |

- **Rumble is on/off only**: wiiuse switches the motor, which has no intensity levels; the intensity is thresholded.
- **The last rumble request wins**: `trigger` and `pulse` replace whatever is ongoing. A trigger cuts a pulse short (`trigger 1` keeps the motor on, `trigger 0` stops it), and a pulse ends a trigger's rumble when it runs out, or restarts a running pulse's timer.
- **Timing**: a request takes effect on the device's next poll (~10 ms), plus the write to the remote (~15 ms on Windows). The end of a pulse is checked on each poll as well, so pulses run up to ~10 ms longer than requested, and very short ones (below ~20 ms) may hardly be felt. (`mote/rumble` was renamed to `mote/rumble/trigger` on 2026-10-19.)
- **Connection rumble**: the hub's short rumble on connect runs as usual until the first request for that remote. Requests for ids that aren't connected are dropped, and a remote that reconnects starts with the motor off.
- **Quitting** (Ctrl+C) switches off the motor of any remote still rumbling, and its speaker.
- **Speaker**: uses `wiimosc-speaker`'s defaults, which played best: ADPCM at the speaker's native 6000 Hz, full volume, asynchronous writes. See [Speaker](Speaker.md).
  - **One sound at a time:** each request replaces the current sound.
  - **Setup:** the first request for a remote sets up its speaker (~0.3 s), which stays on until the remote disconnects, so later sounds start right away.
  - **Silence:** between sounds the speaker sends nothing, which costs no airtime.
  - **Files** are decoded, resampled and normalized when the message arrives, on the receiver's thread. Meanwhile, other OSC input waits: ~50 ms for a short clip (~250 ms for the first one), ~2 s for a 4-minute song. The terminal prints each file played and its decoding time, or why it can't be played.
  - **Statistics:** while sounds play, the speaker prints its statistics line every 5 s and when a sound ends (resyncs mean gaps).
  - **Scanning:** in the streamer, scanning for remotes didn't cause dropouts on the development machine (14 s sine, 0 resyncs). In `wiimosc-speaker`, it does. A likely reason is the streamer's input traffic: motion sensing keeps the link busy, which may keep it out of a low-power mode. That's not verified. Use `--scan-timeout` if sounds drop out.
  - **Board:** the Balance Board has no speaker; requests for its id are ignored as "no Wiimote connected".
- **Ignored input** is reported once per address in the terminal (`[osc] ignoring /wiimosc/0/mote/led: unknown address`): unknown addresses (including another prefix), ids above 7, ids without a connected Wiimote (speaker only), and a missing or wrong argument. Malformed packets are reported as such. Bundles are unpacked, and their time tags ignored.
- The port is bound on all interfaces, so any machine on the network can send to it. It must differ from the target port if the target is on the same machine.
- Testing: `python tools/oscsend.py 4322 /wiimosc/0/mote/rumble/pulse 1 300`, or `python tools/oscsend.py 4322 /wiimosc/0/mote/speaker/file C:/sounds/click.wav` (values that aren't numbers are sent as strings; in Git Bash, prefix it with `MSYS_NO_PATHCONV=1`, otherwise the address is turned into a Windows path).

## Notes & caveats

- **IR** (`--ir`): the camera tracks up to 4 IR sources (a sensor bar has two, at each end).
  - **Positions are only sent while valid:** without visible sources, wiiuse reports 0,0. Use `mote/ir/count`, which is sent before the positions, to tell whether `xyz` is current.
  - **`mote/ir/dot/N`:** the camera's raw position of source N (1024×768, mapped to −1..1 with y flipped; not roll-corrected). It works for any arrangement of sources. Slot numbers are the camera's own, so a source can move to another slot when sources appear or vanish.
  - **`mote/ir/xyz`:** wiiuse's pointer, meant for a sensor bar. x, y are the roll-corrected average of the visible sources; when one source of a pair is lost, they're estimated from the other. They're scaled to wiiuse's 4:3 virtual screen (sensor bar "above"), and keep their last value when the pointer leaves that screen. z is wiiuse's `ir.z`, 1023 minus the pixel distance between the first two sources, divided by 1023: it grows with the distance to the bar, but isn't proportional to it, and isn't in metres. It keeps its last value while one source of the pair is visible, and is 0 until two sources were seen. (This address was `mote/ir/xy`, with x and y only, until 2026-10-18.)
  - Both use the same orientation: x grows when the remote points further right (the sources then appear further left, as wiiuse's raw x grows to the left; measured on a recording, see [Fusion.md §10](Fusion.md#10-ir-anchors)), y when it points further up.
  - **Yaw:** while sources are visible, wiiuse also computes the yaw in `mote/rpy` from them.
  - Enabling the camera blocks that remote's thread for ~100 ms when it connects. With IR on, reports carry IR data, in the basic format with an expansion attached and the extended format without.
- **Yaw** in `mote/rpy` and `chuck/rpy` is 0 without `--ir` (or `--fusion`, for `mote/rpy`). The accelerometers only sense gravity, so they can't measure yaw. Roll and pitch are only reliable while the device isn't accelerating. wiiuse smooths them (`WIIUSE_SMOOTHING`).
- **Gyro**: wiiuse calibrates the MotionPlus on its first reading, so the remote should lie still for a moment after connecting with `--gyro`. Even then, a resting remote can show a small constant rate on one axis (e.g. 0.8–5.8 deg/s roll was observed), which a future sensor fusion step would have to estimate and remove. wiiuse doesn't integrate the rates into an orientation. Enabling the MotionPlus blocks that device's thread for ~0.5 s.
- **MotionPlus vs. Nunchuk** (`--gyro`, `--fusion`): the MotionPlus and a Nunchuk share the expansion port. wiiuse's pass-through mode (both at once) never reads the Nunchuk's calibration, so stick and acceleration values would be off. Switching modes on plug/unplug stalls all devices for ~1 s. Hence `--gyro` is opt-in: without it, Nunchuks work everywhere. Revisit when implementing sensor fusion.
- **Balance Board values** are clamped at 0 kg per sensor by wiiuse (`do_interpolate`), so an empty board reads exactly 0. `--tare` can therefore only remove positive offsets.
  - Don't use `wiiuse_set_wii_board_calib()` for taring: it writes calibration data into the board's memory.
- **Battery**: the streamer requests a fresh status report on connect, and sends the level when it arrives. A bogus level (0.005) was observed before the wiiuse I/O fixes (see [Pairing](Pairing.md#wiiuse-patches)).
- **Rate**: with motion sensing on, a Wiimote sends roughly 50–100 reports/s (~180 with the MotionPlus active), and accelerometer noise makes `rpy`/`accel` change on nearly every one. That's ~200 messages/s per remote, or ~250 with `--gyro`. Use `--max-rate` to reduce traffic.
- **Accelerometer resolution**: wiiuse is built with its full-precision option (`WITH_ACCEL_10BIT`): steps of ~0.01 g on the Wiimote's x axis, ~0.02 g on y and z, ~0.01 g on the Nunchuk's (it was 0.04 g everywhere before). The calibration is read at full precision as well, so resting values can differ slightly from older builds (e.g. 1.049 → 1.045 g).
- **Stalls**: each device runs on its own thread (see [Pairing](Pairing.md#pair--test-tool-wiimosc-pair)), so wiiuse's blocking calls (connecting, plugging in a Nunchuk, enabling the MotionPlus) only pause that one device.
- **Testing**: only one program should receive on the target port. With two receivers bound to the same port (e.g. Protokol and `oscdump.py`), Windows splits the packets between them, which looks like dropouts.

## Sensor fusion (`--fusion`)

- **Startup**: after the MotionPlus is activated, wiimosc reads its factory calibration, then waits until the remote has been **still for 1 s** (lying on a table, or held steadily). Any movement restarts that wait. It measures the gyro's bias there, and starts with the remote's actual pitch and roll, and **yaw 0 = the heading at that moment**. The terminal shows each step: `fusion: MotionPlus calibration read ...`, then `fusion: ready (gyro bias ...; N reports/s)`. Until then, `mote/rpy` and `mote/quat` aren't sent.
- **Angles**: `mote/rpy` has the same meaning and signs for roll and pitch as without fusion (wiiuse's accelerometer angles), but stays valid while the remote moves. Yaw is positive when turning left (counter-clockwise seen from above). Order: yaw, then pitch, then roll. Pointing straight up or down, yaw and roll can't be told apart (gimbal lock); `mote/quat` has no such problem.
- **Drift**: the accelerometer keeps pitch and roll correct over time. **Yaw is from the gyro alone and drifts** slowly. The bias is re-estimated whenever the remote rests for 3 s, which slows the drift but can't undo what has accumulated. Very slow, steady turns (below ~3 deg/s for several seconds) can be mistaken for bias.
- **Fast movement**: while the remote is swung or shaken (acceleration more than 20% off 1 g), the accelerometer is ignored and the gyro carries the orientation alone. Very vigorous motion can leave pitch and roll off by ~10° (the gyro saturates at ~1700 deg/s). Once the remote is still, they return to the accelerometer's within ~1 s.
- **Gyro scale**: the MotionPlus's stored calibration overstates its sensitivity by 10–30%; wiimosc corrects it to the one measured on two Wiimote Plus (a 360° turn reads 357–364°). A MotionPlus accessory hasn't been measured; if its yaw consistently over- or undershoots, the correction may not fit it.
- **IR**: with `--ir`, the IR messages are unchanged, but `mote/rpy`'s yaw is the fused one, not wiiuse's IR-based one. The IR anchors correct it (next section).
- After the MotionPlus is deactivated and reactivated (e.g. after a reconnect), the startup is repeated.

## IR anchors (`--ir anchor=...`)

With `--ir` and `--fusion`, IR emitters at known directions correct the fused yaw, so it stays right over a wide range of angles, e.g. for pointing at many things in a room. The design and its assumptions are in [Fusion.md §10](Fusion.md#10-ir-anchors).

```
wiimosc-stream --fusion madg --ir anchor=0,-60,60:20
```

- **Anchors**: a comma-separated list of `yaw[:elevation]`, in degrees as in `mote/rpy` (yaw positive to the left, elevation like pitch, positive up). The first is the **home anchor**, a sensor bar (two sources side by side; that's also where a screen would be). The others are **single IR sources**. Without `anchor=`, there's only the home anchor, at yaw 0. Elevations are optional; given, they also rule out sources at other heights (±15°), e.g. reflections on a table.
- **Setting up**: point the remote at each emitter and read the yaw off `mote/rpy` (after syncing to home). Single sources must be more than ~40° apart (and from home), so that one can't be mistaken for another.
- **Syncing**: yaw starts at the heading at startup, as without IR. Once the remote has seen the home anchor, yaw refers to the anchors (the terminal prints `fusion: yaw synced to the home anchor`). Until then, single sources aren't used, since they can't be told apart. Seeing home again with the yaw more than 15° off resets it.
- **Correction**: each anchor in view pulls the yaw towards its own direction, gradually (time constant 0.5 s), and faster while the remote turns, where it's hard to notice. Between anchors, the yaw drifts as without IR. A single source is taken for the nearest anchor within 20°, and ignored otherwise.
- **`mote/ir/anchor`**: the index of the anchor in view (0 = home), or −1. Also useful by itself, e.g. "pointing at element 2". With only one source of the home pair visible, it reports home without correcting.
- **Assumptions**: the remote stays at about the same place (stepping 0.5 m sideways shifts an emitter 4 m away by ~7°), and emitters face the remote. Roll and pitch are never changed by IR.
- Remotes without a MotionPlus just stream the IR messages.

## Implementation

- `src/hub/WiimoteHub`: shared by both tools. It runs pairing, device pickup and each device on separate threads, and handles connection output and rumble/blink feedback. It reports to an `AbstractHubListener` (`connected`, `disconnected`, `event`, `update`), called from each device's own thread.
- `src/osc/OscSender`: a minimal OSC 1.0 encoder (int32, float32, string) plus a UDP socket (Winsock / BSD sockets, `SO_BROADCAST` set). No external dependency; OSC messages are simple enough.
- `src/osc/OscReceiver`: the counterpart for `--in-port`. A parser for OSC 1.0 messages and bundles (all standard argument types, numbers read as such), and a UDP socket read on its own thread, which passes each message to a handler. The streamer's `received()` runs there: it only stores the request in an atomic per id, and the device's own thread applies it in `update()`, since only that thread may touch its `wiimote_t`.
  - **Exception: speaker requests.** They're applied right away, to a per-id `Speaker` slot guarded by a mutex: a `SoundPlayer` (`src/speaker/SoundSources`) and the `WiimoteSpeaker` that plays it. The speaker's own thread sends the audio, as in `wiimosc-speaker`; wiiuse's Windows writes don't share state between threads. The device's thread only fills the slot on connect and empties it on disconnect.
- `src/speaker` (library `speaker`): speaker setup and streaming, audio decoding, and the sound sources. Shared with `wiimosc-speaker`, see [Speaker](Speaker.md#implementation).
- `src/stream/WiimoteStreamer`: the listener that maps device state to OSC. It reads wiiuse's state on every `update()` rather than reacting to events, and sends a value only if it differs from the last one sent on that address (cached per device). Buttons are checked on every update; continuous values are subject to the rate limit.
- `src/fusion`: sensor fusion, independent of wiiuse. `MotionFusion` (per remote) converts the raw gyro values with `MotionPlusCalibration`, handles the startup at rest, and drives an `AbstractOrientationFilter`: `MadgwickFilter` for now. `IrAnchorCorrection` corrects its yaw with the IR anchors. Details in [Fusion.md](Fusion.md).
- `tools/oscdump.py`: a minimal receiver for testing, `python tools/oscdump.py 4321 20`. `tools/oscsend.py` sends single messages (`--int`, `--string`, `--bundle`, `--host`).

## Status

| Item | State |
|---|---|
| Wiimote Plus: status, battery, buttons (initial state), `rpy`, `accel` | ✅ received and decoded with `oscdump.py` |
| Balance Board: status, battery, initial values, `--tare` | ✅ received (empty board → 0 kg) |
| `--gyro` on two Wiimote Plus | ✅ `mote/gyro` streams |
| Battery on connect via fresh status request | ✅ plausible values (0.35–0.98) |
| Continuous streaming with 2 remotes + board, battery requests every 10 s | ✅ no gaps over 30+ s (~200 msg/s per remote) |
| Nunchuk attached at connect: `chuck/status`, `xy`, `rpy`, `accel` | ✅ received |
| 0-based ids, `fit/corner/*` (empty board → 0) | ✅ received |
| `--id` mapping, reserved ids, shutdown summary, argument validation | ✅ tested with 2 remotes + board (both tools) |
| `--idle-timeout` (resting remote + empty board), `status none` on idle shutdown | ✅ both shut down after the timeout; `status none` received |
| `--ir`: count, xyz, dots, orientation, yaw | ⏳ builds; not yet run on hardware |
| `--ir anchor=...` with `--fusion madg`: sync, correction, `mote/ir/anchor` | ⏳ the first live test with a sensor bar showed yaw jumps (wrong camera x sign); fixed, and verified on a recording of that bar (Fusion.md §10); not yet re-tested live; single sources untested |
| `--fusion madg`: calibration read, startup, `rpy`, `quat`, axis signs | ✅ user-confirmed on a Wiimote Plus; signs and gyro scale verified with recordings |
| `--fusion madg`: tuned scale and accelerometer handling (settling ~1 s after motion) | ⏳ verified offline on recordings from one remote; not yet tried live; second remote's scale unchecked |
| `--in-port`: receiving, parsing (floats, ints, bundles), ignored-input messages, port in use | ✅ tested without a remote, with `oscsend.py` |
| `--in-port`: `mote/rumble` (now `mote/rumble/trigger`) on a remote | ✅ user-confirmed on a remote (2026-10-19) |
| `--in-port`: `mote/rumble/pulse`, overriding between trigger and pulse | ✅ user-confirmed on remotes (2026-10-19) |
| `--in-port`: `mote/speaker/{sine,file,stop}` | ✅ by the statistics (2026-10-19): sine, switching sines, stop, a 2 s file played in full and stopped halfway, invalid requests reported, clean shutdown while playing; 0 resyncs throughout. ⏳ not yet confirmed by ear |
| `--scan-timeout 5` | ✅ scanning stopped at 9.4 s, after the running cycle |
| Button changes, board under load, `--max-rate`, broadcast, reconnect while streaming, Nunchuk plug/unplug | ⏳ not yet confirmed |
