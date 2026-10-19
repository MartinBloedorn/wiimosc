# Development notes

This is a handover for agents continuing work on wiimosc. It collects the technical decisions, the findings behind them (many found the hard way, on hardware), the current state, and open work. The topic docs have more detail: [Pairing.md](Pairing.md) (Bluetooth pairing, hub, wiiuse patches), [Streaming.md](Streaming.md) (OSC tool and address reference), [Fusion.md](Fusion.md) (sensor fusion: design, measurements, tuning) and [Speaker.md](Speaker.md) (speaker prototype). Coding style lives in [../CLAUDE.md](../CLAUDE.md).

Last updated: 2026-10-19.

## 1. State of the project

| Area | State |
|---|---|
| Windows pairing (SYNC and 1+2 flows, re-pairing, stale cleanup) | ✅ working on hardware (SYNC tested; 1+2 untested) |
| Device pickup, connect/disconnect, rumble/blink feedback | ✅ working |
| `wiimosc-pair` (pair & test tool) | ✅ working |
| `wiimosc-stream` (OSC) | ✅ working: Wiimote buttons/rpy/accel/gyro, Nunchuk, Balance Board (empty board only), battery, status |
| `wiimosc-speaker` (speaker prototype) | 🧪 macOS: audible, right pitch and timing, with a ~60 Hz buzz from the link's limit (measured by microphone). Windows: untested since the fixes ([Speaker.md](Speaker.md)) |
| macOS (pairing, input, OSC, fusion) | ✅ works on hardware with a Wiimote Plus (macOS 14.8): SYNC pairing, reconnect by button, streaming, `--shutdown-on-exit`. Input runs at ~67 reports/s instead of ~180 (§9). Board, 1+2, several remotes, speaker untested |
| Linux | ❌ not implemented; CMake stops with `FATAL_ERROR` |
| Sensor fusion (`--fusion madg`: fused rpy with yaw, quaternion) | ✅ works on hardware (user-confirmed); gyro scale and accelerometer handling tuned on recordings of both remotes ([Fusion.md](Fusion.md), §6c) |
| IR anchors (`--ir anchor=...`: yaw correction by IR emitters) | 🧪 camera sign fixed after the first live test, verified on a sensor bar recording; not yet re-tested live (§6c) |

**Development hardware** (user's machine, Windows 10, VS 2022 Community, CMake, Python 3.11):

- Radio: Intel Wireless Bluetooth, `44:85:00:BF:B6:D8`. Its address contains `0x00` and several bytes ≥ `0x80`, which matters for pairing (§3).
- Two Wiimote Plus (`RVL-CNT-01-TR`, built-in MotionPlus): `B8:AE:6E:56:1B:BD`, `8C:CD:E8:C4:D5:0F`. One Nunchuk.
- Balance Board (`RVL-WBC-01`): `00:1F:C5:9E:80:70`.
- The user monitors OSC with **Protokol**, usually on UDP port **12021**.
- Also a **Mac** (macOS 14.8, Apple clang 15, CMake 3.30, Python 3.11): radio `C8:89:F3:AD:64:90` (Broadcom, several bytes ≥ `0x80`). Karabiner's virtual HID devices are installed (harmless).

## 2. Layout & build

```
src/pairing/   WiimotePairer: Bluetooth inquiry + OS-level pairing (WiimotePairer_win.cpp / WiimotePairer_mac.mm),
               Info.plist (Bluetooth usage description, embedded into the macOS executables)
src/hub/       WiimoteHub + AbstractHubListener: threads, device pickup, feedback (shared by both tools)
src/osc/       OscMessage / OscSender: minimal OSC 1.0 encoder + UDP sender (portable)
src/stream/    wiimosc-stream: WiimoteStreamer (listener → OSC) + CLI
src/fusion/    MotionFusion (rest calibration → AbstractOrientationFilter), MadgwickFilter, MotionPlusCalibration,
               IrAnchors (IR anchor yaw correction)
src/record/    wiimosc-record: logs a Wiimote Plus's raw motion (and IR) data to CSV (optionally rumble-cued steps)
src/replay/    wiimosc-replay: runs a recording through MotionFusion, prints the orientation as CSV
data/recordings/  raw motion recordings from real remotes + README (format, content of each)
src/pairtool/  wiimosc-pair: listener that prints A presses + CLI
src/speaker/   wiimosc-speaker: WiimoteSpeaker (speaker protocol, paced sender), Audio (decoding, resampling) + CLI
external/wiiuse/  vendored wiiuse 0.16.0, PATCHED (see §5)
external/Fusion/  xioTechnologies Fusion v1.3.3 (MIT, submodule, unmodified): Madgwick's AHRS in C
tools/         oscdump.py, oscrate.py, timestamp.py (testing helpers, see §8)
doc/           Pairing.md, Streaming.md, Fusion.md, Speaker.md, this file, user's background notes
```

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release      # or Debug: enables wiiuse's WIIUSE_DEBUG log

# macOS (Makefiles, single-config)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release   # or Debug
cmake --build build -j8
```

- wiiuse is built as a static subproject (`add_subdirectory(... EXCLUDE_FROM_ALL)`, examples/tests off), with **`WITH_ACCEL_10BIT` on** (the fork's opt-in full accelerometer precision: Wiimote x 10-bit ≈ 0.01 g steps, y/z 9-bit ≈ 0.02 g, Nunchuk 10-bit; 8-bit was 0.04 g; the calibration is read at full precision too). It only changes values, not units or struct layout, so wiimosc doesn't define `WIIUSE_ACCEL_10BIT` itself; don't use wiiuse's raw `accel` or the `*_ACCEL_*SHIFT` macros without it. Button masks (`WIIMOTE_BUTTON_ALL`) exclude the extra bits. `WIIUSE_STATIC` **must** be defined publicly on the target, or `wiiuse.h` declares `dllimport` and linking fails on MSVC.
- Fusion is built from its `Fusion/` subfolder (library only). **Its headers only compile as C++20** in MSVC (C99 designated initializers), so only the `motionfusion` target is C++20, and only `MadgwickFilter.cpp` includes them. Keep Fusion types out of `src/fusion` headers.
- Libraries: `pairing` (bthprops, ws2_32), `hub` (pairing + wiiuse, hid), `osc` (ws2_32), `motionfusion` (Fusion; no wiiuse dependency). Executables: `wiimosc-pair`, `wiimosc-stream`, `wiimosc-speaker` (+ Media Foundation, winmm; built with `NOMINMAX`, since `windows.h`'s `min`/`max` macros break `std::min`).
- **macOS**: the `pairing` library is Objective-C++ (`enable_language(OBJCXX)`, ARC) and links IOBluetooth + Foundation; wiiuse links IOKit + CoreFoundation; `wiimosc-speaker` uses AudioToolbox (`Audio_mac.cpp`). `pairing` adds a linker option to every executable that uses it, embedding `src/pairing/Info.plist` (`NSBluetoothAlwaysUsageDescription`): macOS kills processes that access Bluetooth without one.
- **Linking fails with `LNK1104` while the exe is running** (often the user's own instance). Never kill the user's process; ask, or build the Debug config to check compilation.

## 3. Pairing (Windows): findings

The details are in [Pairing.md](Pairing.md). The essentials:

- **The PIN is a 6-byte address in reverse byte order** (as stored in `BLUETOOTH_ADDRESS.rgBytes`). With **SYNC**, the PIN is the *host's* address, and the pairing is permanent, so any button reconnects later. With **1+2**, it's the *remote's* address, and the pairing is temporary. With the wrong one, Windows drops the device immediately. The software can't tell which button was pressed, so it's a CLI flag (SYNC is the default; the Balance Board only has SYNC).
- **Dolphin's approach fails here**: `BluetoothAuthenticateDevice` with each address byte widened into a `WCHAR` gives errors 1244/258. The passkey is a string that Windows converts before sending, which mangles `0x00`/`≥0x80` bytes. **What works**: `BluetoothRegisterForAuthenticationEx` + `BluetoothAuthenticateDeviceEx(..., MITMProtectionNotRequired)`, with the callback answering `BLUETOOTH_AUTHENTICATION_METHOD_LEGACY` through `BluetoothSendAuthenticationResponseEx` with **raw bytes** in `BLUETOOTH_PIN_INFO`.
- After authenticating: `BluetoothEnumerateInstalledServices` (makes the pairing stick, per Dolphin), then `BluetoothSetServiceState(HID UUID, ENABLE)`. The HID device node appears asynchronously (driver install on the first pairing).
- **Paired but disconnected devices are ambiguous.** Windows lists them as not connected whether they're off, reconnecting, or in pairing mode again. `stLastSeen` is useless: Windows refreshes it on every inquiry for *all* remembered devices, including ones that are switched off (verified against headphones/speakers). **Presence probe**: an SDP query for the HID record (`WSALookupServiceBeginW`, `NS_BTH`, `LUP_FLUSHCACHE`, context `"(XX:XX:..)"`). It succeeds in ~0.6 s if the device is in pairing mode, and fails with 10108 after ~5 s if it's absent. Reachable → `BluetoothRemoveDevice` and pair again. Unreachable → leave it alone; Windows accepts its reconnection on its own.
  - Skipping paired devices entirely was a bug: a remote put into SYNC mode again then never paired. Re-enabling the HID service of a paired, disconnected device fails ("enabling HID service failed").
  - Cost: ~5 s per paired but switched-off remote per scan cycle (it runs on the pairing thread, so it's harmless).
- At startup, devices that Windows remembers but that are neither connected nor authenticated get removed: they block re-discovery (as in Dolphin).
- Names are filtered by the prefix `Nintendo RVL-` (`RVL-CNT-01`, `RVL-CNT-01-TR`, `RVL-WBC-01`).

## 3b. Pairing (macOS): findings

Details in [Pairing.md](Pairing.md#macos-implementation). The essentials:

- Same model as Windows: macOS pairs and connects the remote as a HID device, and wiiuse opens it through **IOKit HID** (`os_mac_hid.c`, a new wiimosc backend; wiiuse's own IOBluetooth/L2CAP backend doesn't work on recent macOS and isn't built). The address comes from the HID serial number (`8c-cd-e8-c4-d5-0f`).
- **IOBluetooth delivers its delegate callbacks on the main thread's run loop**, from whatever thread the operation was started. The tools' main threads must wait in `WiimotePairer::processEvents()`; a sleeping main thread means an inquiry that finds nothing.
- **The PIN needs private API** (as Dolphin's WiimotePair does): `setUserDefinedPincode:YES` and `IOBluetoothCoreBluetoothCoordinator pairPeer:forType:withKey:` with the raw bytes. The public `replyPINCode:` mangles bytes ≥ `0x80` via `NSString` and failed with `0xe00002bc`.
- Unknown device names come back as the address; only the asynchronous `remoteNameRequest:<delegate>` resolves them (class of device is the fallback). Re-pairing removes the old pairing first (private `-remove`) and waits 1 s.
- `IOBluetoothDevice closeConnection` powers a remote off (`--shutdown-on-exit`, `--idle-timeout`).

## 4. Hub architecture & threading

**Why threads**: wiiuse blocks a lot. It has a synchronous handshake with fixed 500 ms sleeps, a synchronous expansion handshake, synchronous register reads with 5 s timeouts, and enabling the MotionPlus sleeps 500 ms. In the original single-loop design, one blocking device froze all others: the stream stopped, and a connection rumble ran for seconds because its "stop" came late.

- **Pairing thread** (detached): `scanAndPair` in a loop. It owns its own `WiimotePairer` and touches no shared state other than stdout and an atomic flag.
- **Finder thread**: calls `wiiuse_find` every 1 s. It passes **one free slot** per call, so each device starts as soon as its own handshake is done, then loops again right away. Busy slots belong to their device thread, so the finder gives `wiiuse_find` **placeholder structs** instead: marked `WIIMOTE_STATE_CONNECTED`, with a copy of `dev_path`. That way the open devices are skipped without touching live structs.
- **One thread per device**: polls only its own `wiimote_t` (`wiiuse_poll(&wm, 1)`; the ~10 ms read timeout paces the loop), runs the feedback, and calls the listener. When the device disconnects, it reports, calls `wiiuse_disconnect` (closes the handle), and frees the slot under `mMutex`. The finder joins a slot's finished thread before reusing the slot.
- **Listener contract** (`AbstractHubListener`): calls for one id always come from that device's thread, while different ids run concurrently. `id` is **0-based** (the user's request), and ids 0–3 light player LEDs 1–4. `connected` → many `event`/`update` → `disconnected`.
- **Ids are per device, not per slot.** The finder reads the Bluetooth address (`HidD_GetSerialNumberString`, which Windows sets to the address for BT HID devices), then picks an id under `mMutex` (`chooseId`). A fixed id from `--id` (`setFixedId`/`parseFixedId`) wins. Otherwise the device takes the lowest id that is neither in use nor reserved by `--id` (reserved ids are only used as a fallback). The slot index stays internal (wiiuse struct, threads). `stop()` prints the connected devices and a ready-to-paste `--id` line (the user's request, for easy remapping). On macOS the address comes from `wm->dev_serial` (filled by the HID backend); other backends need their own.
- **Devices never switch off on their own while connected** (the Wii console does that). `--shutdown-on-exit` makes `stop()` drop each link via `IOCTL_BTH_DISCONNECT_DEVICE` on the radio handle (`WiimotePairer::disconnect`), which powers the device off but keeps the pairing; see [Pairing.md](Pairing.md#shutting-devices-down). `--idle-timeout <minutes>` does the same per device, from its own thread, after no activity (buttons, motion beyond 0.15 g, Nunchuk stick, board weight; `IdleWatcher` in `WiimoteHub.cpp`). Motion only counts when a listener enabled motion sensing.
- wiiuse's mutable globals are only `g_banner` and `logtarget[]`, both set once at init, so per-device threads are safe. `OscSender::send` (`sendto`) is safe to call concurrently.
- Measured: 2 Wiimote Plus + board, all paired → all connected within ~3.2 s; ~200 OSC msg/s per remote with no gaps.

## 5. wiiuse: patches and behaviour

`external/wiiuse` is **modified**; grep for `wiimosc patch`. Keep this list in sync when changing it (it's also in [Pairing.md](Pairing.md#wiiuse-patches)).

| File | Patch | Why |
|---|---|---|
| `os_win.c` | `wiiuse_os_find` skips device paths open in a connected slot, and fills the next unconnected slot (`reset_slot` first) | Upstream refills from slot 0 and reopens every Wiimote, which clobbers open ones. Needed to pick up devices later. |
| `wiiuse.h` | Windows member `dev_path[256]` | Identifies open devices |
| `wiiuse.c`, `os.h` | `wiiuse_init_wiimote(wm, unid)` factored out of `wiiuse_init` | Resets a reused slot |
| `os_mac_hid.c` (new), `src/CMakeLists.txt` | IOKit HID backend for macOS, replacing `os_mac/` in the build: same find semantics as `os_win.c`; input reports go through a per-device dispatch queue into a report queue, so any thread can read (10 ms timeout) without a run loop; synchronous `IOHIDDeviceSetReport` writes | wiiuse's IOBluetooth backend doesn't work on recent macOS, and can't use remotes the OS paired |
| `wiiuse.h` | macOS members `hid_device`, `hid_reports`, `dev_path[256]`, `dev_serial[64]` | Device state; identifying open devices; the Bluetooth address for the hub |
| `os_win.c` | Writes use their own `OVERLAPPED` and wait for completion (`write_file_sync`, 500 ms, then `CancelIoEx`) | Upstream wrote through the shared `hid_overlap` without waiting. The write's completion then signalled the *next read*, which returned "success" with an unfilled buffer (report id `0xcc` = MSVC's uninitialized stack fill in Debug). This caused 5 s timeouts on every handshake, a MotionPlus never detected, `Unknown event [Code 0x0]` warnings, and bogus battery values. The stack buffer was also used after return (a likely crash source). |
| `os_win.c` | After a read timeout, `CancelIo` is followed by `GetOverlappedResult(..., TRUE)` | Otherwise a late completion writes into a dead stack buffer |
| `io.c` | `wiiuse_wait_report` returns when disconnected; `wiiuse_read_data_sync` gives up after 3 attempts (zeroed output) | Upstream retried forever, a permanent hang when a device stops answering |
| `events.c`, `wiiuse.h` | Motion+ probe once per connection (`WIIMOTE_STATE_MPLUS_PROBED`) | The probe is a sync read that the Balance Board sometimes doesn't answer, so every status report (battery every 10 s) stalled the board for 5 s. Side effect: a Motion+ *accessory* plugged in after connecting isn't detected until reconnect. |

**Upstream oddities left alone:**

- `wiiuse_os_disconnect` returns early when the device *is* connected (looks inverted), so `wiiuse_cleanup` doesn't close the handles of connected devices. That's harmless at exit, and the hub calls `wiiuse_disconnect` only after a disconnect, where it does close the handle.
- `ResetEvent(&wm->hid_overlap)` in `wiiuse_os_disconnect` passes the wrong type; it's harmless.
- The read event is created signalled (`CreateEvent(NULL, 1, 1, "")`).

**Behaviour to know about:**

- **The handshake is synchronous** (`WIIUSE_SYNC_HANDSHAKE`) and runs inside `wiiuse_find`. Events set there, notably `WIIUSE_WII_BOARD_CTRL_INSERTED` and `WIIUSE_NUNCHUK_INSERTED` for devices already attached at connect, are **reset by the next poll and never seen**. Check state instead: `exp.type == EXP_WII_BOARD` / `EXP_NUNCHUK`. Generally, prefer reading state over relying on events.
- **The Balance Board** is a Wiimote (PID `0x0306`) with an "expansion". Its front button is reported as `WIIMOTE_BUTTON_A`. Its LED 1 is the blue power-button LED, and it has no rumble.
- **The board's per-sensor kg values are clamped at 0** (`do_interpolate`), so an empty board reads exactly 0, and a tare can only remove positive offsets. **Never call `wiiuse_set_wii_board_calib()`**: despite its name, it *writes calibration into the board's memory*. `--tare` is done in software (1 s average after connect).
- `wiiuse_motion_sensing(wm, 1)` is needed for accelerometer data (`orient`, `gforce`). Yaw is only available with IR (`wiimosc-stream --ir`), and is 0 otherwise. IR: `wiiuse_set_ir` (blocks ~100 ms); `ir.num_dots`, raw `dot[i].rx/ry` (1024×768), and the pointer `ir.x/y` on the virtual screen `ir.vres` (560×420 by default, 4:3, bar above; keeps its last value off-screen, 0,0 without sources). Default sensitivity level 3 (`WIIMOTE_INIT_STATES`). Smoothing (`WIIUSE_SMOOTHING`) is on by default.
- **MotionPlus**: `wiiuse_set_motion_plus(wm, 1)` (standalone) hides a Nunchuk. Mode 2 (pass-through) never reads the Nunchuk's calibration, so stick/accel values would be off, and wiiuse marks the path as incomplete. Switching modes blocks ~0.5–1 s. Gyro calibration takes the first reading as zero (the remote must be still), and a resting remote can still show a small constant rate (0.8–5.8 deg/s observed). wiiuse gives rates only, not orientation. Its rate conversion (`/20` slow, `/4` fast, plus a 0.5 deg/s dead band) is rough and unsuitable for integration; the fusion uses `raw_gyro` + `acc_mode` instead (§6c).
- `wiiuse_status()` triggers a status report that is handled in poll (`WIIUSE_STATUS` event, `battery_level`). Before the patches, the battery level stored during the handshake was sometimes bogus.
- Before the first input report, values are 0 (e.g. the first `mote/accel` sent is `0,0,0`).

## 6. OSC streaming: decisions

The address reference is in [Streaming.md](Streaming.md).

- **No OSC library**: the hand-written encoder (int32/float32/string, 4-byte padding, big-endian) and UDP sender with `SO_BROADCAST` are ~150 lines and portable.
- **Address scheme** (designed with the user): `<prefix>/<id>/{mote,chuck,fit}/...`, plus device-level `<prefix>/<id>/status` (string `mote`/`fit`/`none`) and `<prefix>/<id>/battery`. The prefix is normalized (`wiimosc` → `/wiimosc`).
- **Booleans are int 0/1** (the user's choice, for receiver compatibility); not OSC `T`/`F`.
- **Change detection**: every address caches its last sent values per device, so only changes are sent. The cache is reset on connect, so a full state dump follows every connection.
- **Rate**: buttons and status messages go out immediately. Continuous values are capped by `--max-rate` Hz per device (0 = unlimited, the default). The streamer reads wiiuse *state* on every `update()` rather than reacting to events, so the latest value is always the one sent.
- **Balance Board**: `weight` (sum), `raw/0..3` (kg, order TL, TR, BL, BR), `corner/0..3` (share of total), `xy` (center of mass, x = right − left, y = top − bottom, normalized). `corner` and `xy` are 0 below 2 kg (`sMinBoardWeight`), to avoid div-by-0 and empty-board noise.
- **Gyro is opt-in** (`--gyro`), as is sensor fusion (`--fusion`, §6c): the user rarely needs raw rates. That avoids the Nunchuk conflict by default. Revisit pass-through (after reading the Nunchuk's calibration first) when doing fusion.
- `--host 0.0.0.0` is rejected: it's a listening address, and `sendto` fails on Windows. The default target is `127.0.0.1`.
- Battery: sent on connect (after a fresh `wiiuse_status`) and every 10 s.

## 6b. Speaker: findings

Details are in [Speaker.md](Speaker.md).

- wiiuse has no speaker support (it only reads the "speaker enabled" status bit). The protocol (reports `0x14`/`0x19`/`0x18`, the registers at `0x04a20001`–`09`, Yamaha ADPCM) is implemented in `WiimoteSpeaker`.
- **A Wiimote Plus ignores the speaker's rate register: it always plays ~3000 bytes/s (ADPCM ~6000 Hz, PCM ~3000 Hz), i.e. 150 reports/s.** This was measured with the MacBook's microphone and FFTs, and is why the old 2000 Hz default produced only chopped noise.
  - The defaults are now these native rates. The sender skips the source samples it can't send, so pitch and duration are right, with gaps.
  - The PCM volume is silent from `0x80` (WiiBrew: `0xff`); the maximum is now `0x40` for both formats.
- **The link carries ~61 (Windows) / ~67 (macOS) reports/s at most.** On the MS stack, `WriteFile` takes ~15 ms to complete, and asynchronous writes with several in flight don't raise the ceiling. A pairing inquiry during playback costs throughput, so `wiimosc-speaker` pauses scanning while its target is connected (`WiimoteHub::setPairingPaused`).
- Audio reports go through the tool's own overlapped writer on `wm->dev_handle`, concurrently with the device thread's reads. That works because every write has its own `OVERLAPPED` (as in the `write_file_sync` patch).
- `timeBeginPeriod(1)` is needed for the 16–20 ms report period.
- On macOS, every `--writer` sends through wiiuse (synchronous `IOHIDDeviceSetReport`); `AsyncWriter` is Windows-only. Decoding uses AudioToolbox (`ExtAudioFile`; checked with aiff and m4a). Untested on hardware.

## 6c. Sensor fusion: summary

The full reference (design, conventions, measurements, tuning method, open work) is **[Fusion.md](Fusion.md)**. The essentials:

- **`--fusion <madg|off>`** (default off = stock wiiuse behaviour) activates the MotionPlus like `--gyro` (so it hides a Nunchuk), replaces `mote/rpy` with fused angles including yaw, and adds `mote/quat`. Remotes without MotionPlus keep wiiuse's `mote/rpy`; `--gyro` stays independent.
- **Structure**: `src/fusion` (no wiiuse dependency). `MotionFusion` per remote converts raw values (`MotionPlusCalibration`), measures bias, gravity and report rate during 1 s of rest, then drives an `AbstractOrientationFilter` (`MadgwickFilter`, on xioTechnologies Fusion). `IrAnchorCorrection` corrects its yaw. An EKF plugs in as another filter; IR measurements (`IrFrame`) would then go into `MotionSample`.
- **Gyro sensitivity**: the MotionPlus's stored scales make the gyro over-read by 10–30%. The measured sensitivity, ~20 counts per deg/s on two Wiimote Plus and unrelated to the stored values, replaces them (`sSlowSensitivity = {20.4, 20.4, 20.0}`). Yaw is now within ±1% per 360° turn.
- **Accelerometer handling**: Fusion's own acceleration rejection is off (it trusted a shaking remote after 5 s, and rejected valid samples at rest). Instead, the accelerometer is used while |a| is within 20% of its rest value, with gain 1.0 while moving and 5.0 at rest: the tilt settles within ~1 s after motion.
- **Samples** come from `event()` on `WIIUSE_EVENT` (~180 reports/s), with dt measured on the host.
- **Tuning on recordings**: `wiimosc-record` / `wiimosc-replay` and `data/recordings/`. Use them again for the EKF.
- **IR anchors** (`--ir [anchor=0,-60,60:20]` with `--fusion`; Fusion.md §10): IR emitters at known yaw (and optional elevation) correct the fused yaw. The first anchor is a sensor bar (home), which can be identified, so it syncs the yaw at startup and recovers it. The others are single sources, matched to the nearest anchor within 20° once synced. `IrAnchorCorrection` keeps a yaw offset on top of the filter's output and nudges it (0.5 s, rate-limited); roll and pitch are never touched. `mote/ir/anchor` reports the anchor in view. There's no separate screen mode: home is where the screen is. `mote/ir/xy` became `mote/ir/xyz` (plus wiiuse's unitless `ir.z`). The first live test showed yaw jumps whenever the bar's second dot came and went: the camera's x (wiiuse's `rx`) grows to the *left*, not the right. Fixed and verified on a recording of sweeps across a bar, which also gave the field of view (39.7°). The simulation had missed it, since it used the code's own camera model: check such assumptions on recordings.

## 7. Conventions & preferences

- Style per [CLAUDE.md](../CLAUDE.md): Qt-style naming (`mMember`, `sStatic`, `AbstractX`, `isX`), 4-space indent, doxygen `/*! \brief ... */` with a single-line brief, and no documentation of trivial parameters. Keep solutions simple.
- The user decides on design questions (they answered: patch wiiuse in place rather than wrap it; int booleans; `--max-rate`; extras); ask when a choice changes behaviour.
- Docs are kept up to date with each change (status tables in Pairing.md / Streaming.md), and test status is reported honestly (✅ only when verified on hardware).

## 8. Testing & debugging

- Everything real needs hardware; the user keeps devices on for testing. Ask the user for physical actions (press SYNC, power-cycle, stand on the board, plug in a Nunchuk). In SYNC mode, a remote stays discoverable for ~20 s.
- **Only one wiimosc instance at a time**: two instances open the same HID devices and steal each other's input reports. Check with `Get-Process wiimosc-stream` (macOS: `pgrep -l wiimosc`).
- **Only one receiver per UDP port**: with two sockets bound to the same port (e.g. Protokol and a test script on 12021), Windows splits the packets between them, which looks like dropouts. Test on a free port (4321) and check `Get-NetUDPEndpoint -LocalPort <port>` (macOS: `lsof -nP -iUDP:<port>`).
- Redirected stdout is fully buffered, so both tools call `setvbuf(stdout, nullptr, _IONBF, 0)`; keep that.
- Tools:
  - `python tools/oscdump.py <port> <seconds>`: per-address counts and first/last decoded values.
  - `python tools/oscrate.py <port> <seconds>`: messages per second per device id, to spot stalls.
  - `python tools/timestamp.py [--interrupt] <seconds> <cmd...>`: timestamps every output line. Combined with the **Debug** build (which enables wiiuse's `WIIUSE_DEBUG` log), this shows where time goes; that's how the 5 s read timeouts and `0xcc` buffers were found. `--interrupt` ends the run with Ctrl+Break (a clean shutdown, so the shutdown summary prints) instead of a kill.
- On macOS, `timestamp.py --interrupt` sends SIGINT. IOBluetooth logs lines like `-[IOBluetoothDeviceInquiry initWithDelegate:]` to stderr on every inquiry; filter them out with `grep -v`.
- Pattern for timed runs from PowerShell: `Start-Process ... -RedirectStandardOutput ... -PassThru`, sleep, then `Stop-Process` (a hard kill: no shutdown output). On Windows, a script can't send Ctrl+C to a single process, only Ctrl+Break, which the tools handle like Ctrl+C.
- Run Python with `-I` (isolated mode).

## 9. Open work & known issues

- **Unverified on hardware**: `--ir` streaming over OSC (the camera's axes are measured: x grows when pointing right), and the IR anchors live after the sign fix (Fusion.md §10 lists the checks); 1+2 pairing; power-cycling a remote *while streaming*; board under load (weight, corners, xy, tare offsets); button changes over OSC (buttons were verified in `wiimosc-pair`); `--max-rate`; broadcast targets; Nunchuk plug/unplug while streaming; `--gyro` together with a Nunchuk.
- **Linux backend**: wiiuse's BlueZ path connects over L2CAP without pairing. For permanent SYNC pairing, BlueZ's `wiimote` plugin supplies the PIN, so pairing could go over D-Bus (`Device1.Pair`, `Trusted`). Check the kernel `hid-wiimote` driver claiming devices. The user couldn't get wiiuse working on Linux before.
- **macOS**: input arrives at **~67 reports/s** (Windows: ~180), i.e. every ~15 ms, most likely because macOS puts the HID link into Bluetooth sniff mode. The fusion measures dt and works, but with less gyro resolution in time; there's no public API to change the sniff interval. Untested on macOS: 1+2 pairing, the Balance Board, several remotes at once, `--idle-timeout`, the speaker, a Nunchuk. The private pairing API may disappear in a future macOS (the tool then warns and falls back to the public PIN reply, which fails for addresses with bytes ≥ `0x80`).
- No rumble on connect was noticed with `--fusion` (seen on macOS; enabling the MotionPlus right after connecting may cut it short). Not checked on Windows.
- The `os_win.c` / `os_mac_hid.c` find/I/O patches are platform-specific; a Linux backend would need equivalents (re-scan without clobbering, per-device threads).
- **Sensor fusion**: the user hasn't yet tried the tuned version live. Tilt errors of ~8–14° can still build up during very vigorous motion (accelerometer unusable, gyro saturating); they settle within ~1 s at rest. Next steps (IR anchors on hardware, EKF, 10-bit motion recordings, MotionPlus accessory, yaw re-zeroing, Nunchuk pass-through): [Fusion.md §11](Fusion.md#11-open-work).
- Without `--id`, a device that reconnects may get a different id (lowest free one).
- Each paired but switched-off remote adds ~5 s to every pairing scan cycle (presence probe); the probes could run in parallel.
- Shutdown joins device threads; a thread stuck in a sync read can delay exit by up to ~15 s (3 × 5 s).
- **Speaker**: buzzes at the link's report rate (~44% duty), since the speaker needs 150 reports/s; getting the link out of sniff mode might help (not attempted). Untested: the new defaults on Windows, an original (non-Plus) Wiimote (might honour the rate register), trigger mode, mp3, reconnecting while playing.
