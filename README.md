# wiimosc

Cross-platform utility that pairs Nintendo Wiimotes (with Nunchuk) and the Wii Fit Balance Board, and turns their input into OSC messages.

> **Status:** early. Automated pairing and OSC streaming work on Windows and macOS; on Linux they work without pairing (press SYNC or 1+2 every session). See [doc/Pairing.md](doc/Pairing.md) and [doc/Streaming.md](doc/Streaming.md).

## Why

Getting a Wiimote to talk to a computer normally means pairing it through the OS with a binary PIN derived from a MAC address. wiimosc does this automatically: it scans for remotes, pairs them, and hands them to [wiiuse](https://github.com/wiiuse/wiiuse) for input.

## Building

Requirements: CMake ≥ 3.16 and a C++17 compiler (Visual Studio 2022 on Windows, Xcode's command-line tools on macOS, GCC or Clang on Linux with the development packages of GLib/GIO and libsndfile, e.g. `glib2-devel libsndfile-devel` on Fedora, `libglib2.0-dev libsndfile1-dev` on Debian/Ubuntu). wiiuse is included under `external/wiiuse` and is built as a static library.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

On Linux, bluetoothd must be running; it only does the discovery. Remotes aren't paired (see [doc/Pairing.md](doc/Pairing.md#linux-implementation)), so press SYNC or 1+2 each time a remote should connect.

On macOS, the first run may ask to allow Bluetooth access for your terminal app (System Settings → Privacy & Security → Bluetooth).

## Usage

Both tools scan continuously in the background and pair any Nintendo device in pairing mode. Pairing defaults to the red SYNC button, which gives a permanent pairing and is required for the Balance Board; `--one-two` pairs remotes held at 1+2 instead. Use the option that matches the button you pressed: with the wrong key, the OS drops the remote immediately.

Every device that connects gets a line in the terminal, whether it was just paired, woke up again, or was already connected at startup. Wiimotes also get their player LED and a short rumble, and the Balance Board blinks its power LED. Press Ctrl+C to quit.

Devices don't switch off by themselves while connected: on a Wii, the console does that, and the Balance Board has no power button of its own. With `--shutdown-on-exit` (all tools), quitting drops every connected device's Bluetooth link, which switches it off; a button press brings it back. `--idle-timeout <minutes>` does the same for devices that have gone unused for that long. `--scan-timeout <seconds>` (all tools) stops scanning for new remotes after that long, which frees the radio for the connected ones (e.g. for the speaker); remotes paired before still reconnect.

### `wiimosc-stream`: OSC output

```
wiimosc-stream --host 127.0.0.1 --port 4321 --prefix wiimosc
```

This streams every device's input to the OSC target, for example:

```
/wiimosc/0/status          "mote"
/wiimosc/0/mote/button/a   1
/wiimosc/0/mote/rpy        -2.3 2.3 0.0
/wiimosc/0/chuck/xy        0.12 -0.98
/wiimosc/1/fit/weight      71.4
/wiimosc/1/fit/corner/0    0.27
/wiimosc/1/fit/xy          0.05 -0.21
```

Further options: `--max-rate <hz>`, `--gyro` (MotionPlus), `--fusion` (sensor fusion on Wiimote Plus remotes: orientation including yaw, and a quaternion; the remote rumbles once calibrated), `--ir` (IR camera: pointer and IR source positions; with `--fusion`, `--ir anchor=0,-60,60` corrects the yaw with IR emitters at known directions, a sensor bar first), `--tare` (Balance Board), OSC input (`--in-port 4322`: `/wiimosc/0/mote/rumble/trigger 1` switches a remote's rumble on, `.../rumble/pulse 1 300` rumbles for 300 ms; `/wiimosc/0/mote/speaker/sine 1000`, `.../speaker/file <path>` and `.../speaker/stop` play sounds on its speaker), broadcast targets, and fixed ids per controller (`--id 0=B8:AE:6E:56:1B:BD`, also in `wiimosc-pair`; on Ctrl+C, both tools print the `--id` arguments for the connected controllers, ready to paste). See [doc/Streaming.md](doc/Streaming.md) for all addresses and options. To check the output without other software: `python tools/oscdump.py 4321 20`.

### `wiimosc-pair`: pairing & testing

Pairs devices like `wiimosc-stream`, and prints a line for each press of A on a Wiimote, or of the Balance Board's front button:

```
[0] Wiimote Plus B8:AE:6E:56:1B:BD connected
[1] Balance Board 00:1F:C5:9E:80:70 connected
[0] Wiimote Plus: A pressed
[1] Balance Board: button pressed
^C
connected devices:
  [0] Wiimote Plus  B8:AE:6E:56:1B:BD
  [1] Balance Board 00:1F:C5:9E:80:70
to keep these ids: --id 0=B8:AE:6E:56:1B:BD --id 1=00:1F:C5:9E:80:70
```

### `wiimosc-speaker`: speaker tests (prototype)

Pairs devices like the other tools, and plays a sine, a file (wav, mp3, m4a, ...), or a clip triggered by A on one remote's speaker:

```
wiimosc-speaker --target-id 1 --test-mode sine
wiimosc-speaker --target-id 1 --test-mode file --file song.m4a
wiimosc-speaker --target-id 1 --test-mode trigger --file click.wav
```

The speaker is lo-fi by nature, and always plays at ~6000 Hz (ADPCM). That takes 150 audio reports/s, which a free radio carries on Windows, but which scanning for remotes disrupts. On macOS, the link carries only ~67/s, so there it buzzes. See [doc/Speaker.md](doc/Speaker.md) for options, protocol and findings. `wiimosc-stream` can play sounds too, triggered over OSC (see above).

## Layout

| Path | Contents |
|---|---|
| `src/pairing/` | `WiimotePairer`: scanning and OS-level pairing, with one source file per platform |
| `src/hub/` | `WiimoteHub`: background pairing, device pickup, connection feedback; shared by both tools |
| `src/osc/` | Minimal OSC encoder, parser, and UDP sender/receiver |
| `src/stream/` | `wiimosc-stream` command-line tool |
| `src/pairtool/` | `wiimosc-pair` command-line tool |
| `src/speaker/` | Speaker protocol, audio decoding and resampling (library `speaker`, also used by `wiimosc-stream`), and the `wiimosc-speaker` command-line tool |
| `src/fusion/` | Sensor fusion for `--fusion` ([details](doc/Fusion.md)) |
| `src/record/`, `src/replay/` | `wiimosc-record` / `wiimosc-replay`: record a Wiimote Plus's raw motion data, and replay it through the sensor fusion |
| `data/recordings/` | Raw motion recordings from real remotes, for tuning the fusion offline ([format](data/recordings/README.md)) |
| `tools/` | Testing helpers: `oscdump.py` (OSC receiver), `oscrate.py` (message rates), `oscsend.py` (OSC sender), `timestamp.py` (timestamped output) |
| `external/wiiuse/` | wiiuse library, patched locally to pick up devices that connect later ([details](doc/Pairing.md#wiiuse-patches)) |
| `external/Fusion/` | [xioTechnologies Fusion](https://github.com/xioTechnologies/Fusion) (Madgwick's AHRS), unmodified |
| `doc/` | [Development notes](doc/DevelopmentNotes.md) (start here), [pairing](doc/Pairing.md), [streaming](doc/Streaming.md), [sensor fusion](doc/Fusion.md) and [speaker](doc/Speaker.md) internals, [background notes on Wiimotes for A/V](doc/Wiimotes%20for%20AV-interactions.md) |

## Platform support

| | Pairing | Input (wiiuse) | OSC |
|---|---|---|---|
| Windows | ✅ | ✅ | ✅ |
| Linux | 🧪 discovery only, no pairing: SYNC or 1+2 every session (BlueZ over D-Bus) | 🧪 wiiuse over plain L2CAP; ~60 reports/s | ✅ |
| macOS | ✅ (IOBluetooth) | ✅ (IOKit HID backend; ~67 reports/s) | ✅ |
