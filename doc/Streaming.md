# OSC streaming

`wiimosc-stream` pairs and connects devices the same way `wiimosc-pair` does (see [Pairing](Pairing.md)), and streams their input as OSC over UDP.

```
wiimosc-stream --host 127.0.0.1 --port 4321 --prefix wiimosc [--max-rate 60] [--gyro] [--tare]
```

| Option | Default | Meaning |
|---|---|---|
| `--host` | `127.0.0.1` | Target: host name, IPv4 address, or broadcast address (`255.255.255.255`, `192.168.1.255`). `0.0.0.0` is rejected, since it's a listening address. |
| `--port` | `4321` | Target UDP port |
| `--prefix` | `wiimosc` | Address prefix. Normalized: `wiimosc`, `/wiimosc/` → `/wiimosc`; may be empty. |
| `--max-rate` | `0` | Max. rate (Hz) of continuous values per device; `0` = unlimited. Buttons and status messages are never limited. |
| `--gyro` | off | Activates the MotionPlus on Wiimote Plus remotes and streams `mote/gyro`. A Nunchuk on such a remote is then not read. |
| `--tare` | off | Zeroes the Balance Board after connecting: averages its sensors for 1 s, keep it empty. |
| `--ir` | off | Activates the Wiimotes' IR cameras, and streams the IR sources they see (`mote/ir/...`). |
| `--ir-sensitivity <1-5>` | `3` | IR camera sensitivity (wiiuse's presets; 5 picks up the faintest sources) |
| `--one-two`, `--no-auth` | | Pairing method, as for `wiimosc-pair` |
| `--id <id>=<address>` | | Always gives the device with this Bluetooth address this id (0–7). Repeatable; also available in `wiimosc-pair`. |
| `--shutdown-on-exit` | off | Switches off all connected devices when quitting (receivers get `status none` for each). See [Pairing](Pairing.md#shutting-devices-down). |
| `--idle-timeout <minutes>` | `0` (never) | Switches off devices that have been idle this long: no button changes, no motion, no Nunchuk stick movement, and an empty, unchanged Balance Board. Receivers get `status none`. Fractions work (`0.5`). |

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
- The address comes from the HID serial number, which Windows sets to the Bluetooth address for Bluetooth HID devices. A device whose address can't be read is shown as `(unknown address)` and always gets a free id.

| Address | Arguments | Sent |
|---|---|---|
| `<p>/status` | string `mote` / `fit` / `none` | on connect / disconnect (including shutdowns by `--idle-timeout` and `--shutdown-on-exit`) |
| `<p>/battery` | float 0–1 | on connect, then every 10 s |
| `<p>/mote/button/{a,b,1,2,plus,minus,home,up,down,left,right}` | int 0/1 | on change |
| `<p>/mote/rpy` | 3 floats, degrees: roll, pitch, yaw | continuous |
| `<p>/mote/accel` | 3 floats, g: x, y, z | continuous |
| `<p>/mote/gyro` | 3 floats, deg/s: roll, pitch, yaw rates | continuous, only with `--gyro` on a Wiimote Plus |
| `<p>/mote/ir/count` | int 0–4 | only with `--ir`; on change: number of IR sources the camera sees |
| `<p>/mote/ir/xy` | 2 floats, −1–1 | only with `--ir`; continuous while `count` > 0: pointer position computed by wiiuse (x right, y up) |
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

## Notes & caveats

- **IR** (`--ir`): the camera tracks up to 4 IR sources (a sensor bar has two, at each end).
  - **Positions are only sent while valid:** without visible sources, wiiuse reports 0,0. Use `mote/ir/count`, which is sent before the positions, to tell whether `xy` is current.
  - **`mote/ir/dot/N`:** the camera's raw position of source N (1024×768, mapped to −1..1 with y flipped; not roll-corrected). It works for any arrangement of sources. Slot numbers are the camera's own, so a source can move to another slot when sources appear or vanish.
  - **`mote/ir/xy`:** wiiuse's pointer, meant for a sensor bar. It's the roll-corrected average of the visible sources; when one source of a pair is lost, it's estimated from the other. It's scaled to wiiuse's 4:3 virtual screen (sensor bar "above"), and keeps its last value when the pointer leaves that screen.
  - Both use the same orientation. Whether x grows when pointing right hasn't been confirmed on hardware yet.
  - **Yaw:** while sources are visible, wiiuse also computes the yaw in `mote/rpy` from them.
  - Enabling the camera blocks that remote's thread for ~100 ms when it connects. With IR on, reports carry IR data, in the basic format with an expansion attached and the extended format without.
- **Yaw** in `mote/rpy` and `chuck/rpy` is 0 without `--ir`. The accelerometers only sense gravity, so they can't measure yaw. Roll and pitch are only reliable while the device isn't accelerating. wiiuse smooths them (`WIIUSE_SMOOTHING`).
- **Gyro**: wiiuse calibrates the MotionPlus on its first reading, so the remote should lie still for a moment after connecting with `--gyro`. Even then, a resting remote can show a small constant rate on one axis (e.g. 0.8–5.8 deg/s roll was observed), which a future sensor fusion step would have to estimate and remove. wiiuse doesn't integrate the rates into an orientation. Enabling the MotionPlus blocks that device's thread for ~0.5 s.
- **MotionPlus vs. Nunchuk**: the MotionPlus and a Nunchuk share the expansion port. wiiuse's pass-through mode (both at once) never reads the Nunchuk's calibration, so stick and acceleration values would be off. Switching modes on plug/unplug stalls all devices for ~1 s. Hence `--gyro` is opt-in: without it, Nunchuks work everywhere. Revisit when implementing sensor fusion.
- **Balance Board values** are clamped at 0 kg per sensor by wiiuse (`do_interpolate`), so an empty board reads exactly 0. `--tare` can therefore only remove positive offsets.
  - Don't use `wiiuse_set_wii_board_calib()` for taring: it writes calibration data into the board's memory.
- **Battery**: the streamer requests a fresh status report on connect, and sends the level when it arrives. A bogus level (0.005) was observed before the wiiuse I/O fixes (see [Pairing](Pairing.md#wiiuse-patches)).
- **Rate**: with motion sensing on, a Wiimote sends roughly 50–100 reports/s, and accelerometer noise makes `rpy`/`accel` change on nearly every one. That's ~200 messages/s per remote, or ~250 with `--gyro`. Use `--max-rate` to reduce traffic.
- **Stalls**: each device runs on its own thread (see [Pairing](Pairing.md#pair--test-tool-wiimosc-pair)), so wiiuse's blocking calls (connecting, plugging in a Nunchuk, enabling the MotionPlus) only pause that one device.
- **Testing**: only one program should receive on the target port. With two receivers bound to the same port (e.g. Protokol and `oscdump.py`), Windows splits the packets between them, which looks like dropouts.

## Implementation

- `src/hub/WiimoteHub`: shared by both tools. It runs pairing, device pickup and each device on separate threads, and handles connection output and rumble/blink feedback. It reports to an `AbstractHubListener` (`connected`, `disconnected`, `event`, `update`), called from each device's own thread.
- `src/osc/OscSender`: a minimal OSC 1.0 encoder (int32, float32, string) plus a UDP socket (Winsock / BSD sockets, `SO_BROADCAST` set). No external dependency; OSC messages are simple enough.
- `src/stream/WiimoteStreamer`: the listener that maps device state to OSC. It reads wiiuse's state on every `update()` rather than reacting to events, and sends a value only if it differs from the last one sent on that address (cached per device). Buttons are checked on every update; continuous values are subject to the rate limit.
- `tools/oscdump.py`: a minimal receiver for testing, `python tools/oscdump.py 4321 20`.

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
| `--ir`: count, xy, dots, orientation, yaw | ⏳ builds; not yet run on hardware |
| Button changes, board under load, `--max-rate`, broadcast, reconnect while streaming, Nunchuk plug/unplug | ⏳ not yet confirmed |
