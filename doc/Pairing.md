# Automated Wiimote pairing

This page records how wiimosc pairs Wiimotes and Balance Boards without the OS pairing dialogs, the decisions behind it, and what is still untested.

## Problem

wiiuse does not pair devices on any platform:

- **Windows** (`os_win.c`): only lists HID devices that are already present, i.e. already paired through the OS.
- **Linux** (`os_nix.c`): runs its own scan (`hci_inquiry`) and connects directly over L2CAP, so it skips OS pairing. The connection lasts only until the remote powers off.
- **macOS** (`os_mac/`): scans with IOBluetooth and opens L2CAP channels directly. This no longer works on recent macOS versions (see [Wiimotes for AV-interactions](Wiimotes%20for%20AV-interactions.md)).

Pairing through the OS by hand means converting MAC addresses into binary PINs. On Windows it fails whenever the address contains a `0x00` byte.

## Pairing key

The PIN is a 6-byte Bluetooth address in reverse byte order, which is how Windows (`BLUETOOTH_ADDRESS.rgBytes`) and BlueZ (`bdaddr_t`) store addresses anyway. Which address is used depends on how the remote was made discoverable:

| Button | Passkey | Result |
|---|---|---|
| Red **SYNC** | the **host** radio's address | Permanent pairing; any button reconnects the remote later. The only option for the Balance Board. |
| **1+2** | the **remote's** own address | Temporary; has to be redone after every power cycle. |

If the tool uses the wrong key, Windows drops the connection immediately. The software can't tell which button was pressed, so the user has to state it (`--one-two`; SYNC is the default).

## Windows implementation

Found in `src/pairing/WiimotePairer_win.cpp`. It follows the flow Dolphin ships in [`Source/Core/Core/HW/WiimoteReal/IOWin.cpp`](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Core/HW/WiimoteReal/IOWin.cpp):

1. `BluetoothFindFirstRadio` + `BluetoothGetRadioInfo` → the host address.
2. **Remove stale devices**: Nintendo devices that Windows remembers but that are neither connected nor authenticated get deleted with `BluetoothRemoveDevice`. Otherwise they block re-pairing.
3. **Scan**: `BluetoothFindFirstDevice` with `fIssueInquiry` and `cTimeoutMultiplier = 3` (×1.28 s). Results are filtered by names starting with `Nintendo RVL-` (`RVL-CNT-01` Wiimote, `RVL-CNT-01-TR` Wiimote Plus, `RVL-WBC-01` Balance Board).
4. **Handle devices that are already known**:
   - Connected devices are skipped.
   - Authenticated but disconnected devices are ambiguous. Windows lists them (with a fresh `stLastSeen`) whether they're off, reconnecting, or in pairing mode again. Neither the flags nor the timestamps tell these cases apart; this was verified against other paired devices that were switched off.
   - A **presence probe** settles it: an SDP query for the HID service record (`WSALookupServiceBeginW`, `NS_BTH`, `LUP_FLUSHCACHE`, context `"(XX:XX:XX:XX:XX:XX)"`). It opens a real connection to the device.
     - The probe **succeeds** in ~0.6 s when the remote is in SYNC or 1+2 mode. The remote wants a new pairing, so the tool deletes the old one with `BluetoothRemoveDevice` and pairs again (steps 5–7). Skipping it instead means the remote never pairs again: in pairing mode it waits for the host and never reconnects on its own.
     - The probe **fails** with WSA error 10108 after the ~5 s page timeout when the device is off. The tool skips it, and a remote that reconnects on a button press is accepted by Windows on its own.
   - Cost: each paired but switched-off Wiimote adds ~5 s to every scan cycle. If this becomes a problem, the probes can run in parallel.
   - Re-enabling the HID service of such a device without re-pairing fails. That was the "enabling HID service failed" error seen after power-cycling the remote with an earlier version.
5. **Authenticate** (if not already authenticated): `BluetoothRegisterForAuthenticationEx` registers a callback for the device, then `BluetoothAuthenticateDeviceEx(nullptr, radio, &info, nullptr, MITMProtectionNotRequired)` starts pairing. The callback answers the `BLUETOOTH_AUTHENTICATION_METHOD_LEGACY` request with `BluetoothSendAuthenticationResponseEx`, passing the 6 address bytes **raw** in `BLUETOOTH_PIN_INFO`.
   - Dolphin's approach, the deprecated `BluetoothAuthenticateDevice` with each address byte widened into one `WCHAR`, **failed on the development machine** with error 1244 (`ERROR_NOT_AUTHENTICATED`) and 258 (`WAIT_TIMEOUT`). The passkey is a string that Windows converts before sending. The radio `44:85:00:BF:B6:D8` contains `0x00` and several bytes ≥ `0x80`, which most likely don't survive that conversion. The callback route sends the bytes unchanged and paired on the first attempt.
6. `BluetoothEnumerateInstalledServices` right after authenticating makes the pairing persist (according to Dolphin). `ERROR_MORE_DATA` counts as success.
7. **Enable HID**: `BluetoothSetServiceState(..., HumanInterfaceDeviceServiceClass_UUID, BLUETOOTH_SERVICE_ENABLE)`. The device then shows up as a HID device, and wiiuse's `wiiuse_find` can open it. That HID device appears with a delay, since Windows installs a driver on the first pairing. The tester picks it up whenever it appears (see below).

`Method::None` skips steps 5–6 and only enables the HID service. Dolphin's continuous scanning works this way for remotes connected with 1+2.

## Pair & test tool (`wiimosc-pair`)

The tool pairs available devices and reports every connection with feedback on the device. Everything except the button output lives in `src/hub/WiimoteHub`, which `wiimosc-stream` uses as well (see [Streaming](Streaming.md)).

- **Threads**: wiiuse blocks in many places, for up to several seconds: the synchronous connection handshake (`WIIUSE_SYNC_HANDSHAKE`, with fixed 500 ms sleeps), the expansion handshake when a Nunchuk is plugged in, and synchronous register reads (`WIIUSE_READ_TIMEOUT` = 5 s). With a single thread, any of these froze all devices: the stream stopped, and connection rumbles ran for seconds. The hub therefore uses separate threads:
  - **Pairing**: `WiimotePairer::scanAndPair` in a loop, detached (one cycle blocks for ~4 s plus ~5 s per paired but switched-off remote). Ctrl+C doesn't wait for a running inquiry.
  - **Finder**: calls `wiiuse_find` every second (relies on the wiiuse patch below). It gets a single free slot per call, so a device starts as soon as its own handshake is done; after a find, it looks again right away. Busy slots belong to their device thread, so `wiiuse_find` gets stand-in structs instead: marked connected and carrying the same `dev_path`, so the open devices are skipped without touching the live structs.
  - **One thread per device**: polls only its own `wiimote_t`, gives the connection feedback, and calls the listener. When the device disconnects (wiiuse clears `WIIMOTE_STATE_CONNECTED` when a read fails with `ERROR_DEVICE_NOT_CONNECTED`), the thread reports it, closes the handle, and frees the slot.
  - wiiuse's only mutable globals (banner flag, log targets) are set once at startup, so per-device threads are safe. Listener calls for one id come from one thread; different ids run concurrently.
- **Connections**: every device the finder opens gets reported, with its Bluetooth address: after pairing, after a remote wakes up again, and for devices already connected at startup.
- **Ids** are assigned per device, not per slot (the slot index is internal). The address comes from `HidD_GetSerialNumberString` on the open HID handle, since Windows reports a Bluetooth HID device's address as its serial (e.g. `b8ae6e561bbd`; verified for Wiimote Plus and Balance Board). With `--id <id>=<address>` (`WiimoteHub::setFixedId`), a device always gets its id. Others get the lowest id that is neither in use nor reserved. On `stop()`, the hub prints the connected devices plus a ready-to-paste `--id ...` line. Both tools also stop on Ctrl+Break (`SIGBREAK`), which is what scripts can send on Windows.
- **Feedback**: Wiimotes get their player LED (LEDs 1–4 for ids 0–3; ids are 0-based) and a 300 ms rumble. The Balance Board has no rumble motor, so its blue power-button LED (LED 1) blinks instead.
- **Balance Board detection**: wiiuse treats the board as a Wiimote with an expansion. Its handshake runs inside `wiiuse_find`, which is also where `WIIUSE_WII_BOARD_CTRL_INSERTED` gets set, and the next `wiiuse_poll` resets it before it can be seen. The hub therefore checks `exp.type == EXP_WII_BOARD` when a device connects.
- **Buttons**: one line per press of A (`IS_JUST_PRESSED`). The board's front button is reported as the A button.
- wiiuse's `[INFO]` log is silenced; warnings and errors still go to stderr.
- **Shutting devices down** (`--shutdown-on-exit`, all tools): see the next section.
- **Timing** (2 Wiimote Plus + Balance Board, all already paired): all three connected within ~3.2 s of startup. Before the I/O patches below, it took ~19 s, because every handshake hit a 5 s read timeout.

## Shutting devices down

Wiimotes and the Balance Board don't switch off by themselves while connected. On a Wii, the console turns them off after inactivity, or (for the board, which has no power button of its own) when its front button is pressed. On a PC they stay on until their batteries run out. Closing the HID handle isn't enough, because the Bluetooth link stays up.

- `WiimotePairer::disconnect(address)` drops the link with `DeviceIoControl(radio, IOCTL_BTH_DISCONNECT_DEVICE, &BTH_ADDR)`. That IOCTL is declared in the SDK's `bthioctl.h` but not documented in the Win32 docs; DS4Windows uses the same call. The device powers off, and the pairing is kept: a SYNC-paired device reconnects on its next button press.
- With `--shutdown-on-exit` (`WiimoteHub::setShutdownOnExit`), `stop()` stops pairing, disconnects every connected device, then waits up to 3 s for the device threads to notice. Listeners still get `disconnected()`, so `wiimosc-stream` sends `status none`.
- Tested with the Balance Board: disconnected 33 ms after the request, and it didn't reconnect in a second run.

**Idle timeout** (`--idle-timeout <minutes>`, all tools; `WiimoteHub::setIdleTimeout`). Each device thread runs an `IdleWatcher` that compares the device state with the state at its last activity. Activity is:

- a button change (remote, Nunchuk, board);
- remote or Nunchuk acceleration differing by more than 0.15 g on any axis (a resting remote jitters by ~0.04 g);
- Nunchuk stick movement over 0.1;
- a Balance Board weight change of 1 kg or more;
- anything of 2 kg or more on the board.

Plugging in or unplugging an expansion also counts. After the timeout, the device thread drops the link like `--shutdown-on-exit` does, and then sees the disconnection as usual (so the streamer sends `status none`). If dropping the link fails, it tries again after another timeout.

- Motion only counts if a listener enabled motion sensing (`wiimosc-stream` does). In `wiimosc-pair` and `wiimosc-speaker`, a remote that's moved around without pressing buttons times out.
- An object of 2 kg or more left on the board keeps it on.
- Tested with `wiimosc-stream --idle-timeout 0.25`: a resting Wiimote Plus and the empty board both shut down 15 s after connecting, and `oscdump` received `status none` for both.

## wiiuse patches

`external/wiiuse` is modified locally; every change is marked with `wiimosc patch`.

**Picking up devices that connect later.** Out of the box, `wiiuse_os_find` (Windows) fills slots from index 0 and reopens every Wiimote present, which clobbers the remotes already open.

- `os_win.c`: `wiiuse_os_find` skips HID device paths that a connected slot already has open, and fills the next unconnected slot. A reused slot is reset first (`reset_slot`: closes the previous device's handle and event, then resets it to defaults). A device that fails the initial report is released again instead of leaking its handle.
- `wiiuse.h`: new Windows member `dev_path[256]` stores the HID device path.
- `wiiuse.c` / `os.h`: the per-slot initialization moved from `wiiuse_init` into `wiiuse_init_wiimote(wm, unid)`, so `os_win.c` can reset a slot.

**Windows I/O bugs.** These caused 5 s stalls on every connection, a MotionPlus that was never detected, bogus reports (`Unknown event ... [Code 0x0]` warnings, a battery level of 0.005), and probably the crash seen once at startup.

- `os_win.c`, writes: `wiiuse_os_write` issued overlapped `WriteFile`s on the shared `hid_overlap` and never waited for them. A pending write's completion then signalled the *next read*, which returned "success" with an unfilled buffer (seen as report `0xcc`, MSVC's fill pattern for uninitialized stack memory). Synchronous reads waiting for a reply therefore missed it and timed out after 5 s. Also, the write buffer on the stack was still in use after the function returned. Now each write uses its own `OVERLAPPED` and waits for completion (`write_file_sync`, cancelled after 500 ms).
- `os_win.c`, reads: after a read timeout, `CancelIo` didn't wait for the cancellation, so a late completion could write into the caller's buffer after it was gone. Now waits with `GetOverlappedResult(..., TRUE)`.
- `io.c`: `wiiuse_wait_report` returns once the device is disconnected, and `wiiuse_read_data_sync` gives up after 3 attempts (it used to retry forever, which hangs when a device stops answering). On failure, its output is zeroed.
- `events.c` / `wiiuse.h`: the Motion+ probe runs only once per connection (`WIIMOTE_STATE_MPLUS_PROBED`), instead of on every status report for devices without a Motion+. It's a synchronous read, and the battery status requests every 10 s triggered it each time. Side effect: a Motion+ *accessory* plugged in after connecting isn't detected until the remote reconnects.

Noticed but left alone: `wiiuse_os_disconnect` returns early when the remote *is* connected, which looks inverted. As a result, `wiiuse_cleanup` doesn't close the handles of connected remotes. This is harmless at process exit.

## Decisions

- **C++17 + CMake, with wiiuse as a static subproject.** wiiuse already handles HID I/O, the input reports and the extensions (Nunchuk, Balance Board). wiimosc only adds the pairing step that wiiuse lacks.
- **`WIIUSE_STATIC` is defined publicly on the `wiiuse` target.** Without it `wiiuse.h` declares `dllimport`, and static linking fails on MSVC. This was the real cause of the linking failure described in the AV notes, which was worked around there with `BUILD_SHARED_LIBS`.
- **Pairing is a separate `pairing` library** (`WiimotePairer`) with one source file per platform. The header is platform-neutral, and only the Windows backend exists so far.
- **SYNC is the default method**: permanent pairings mean less hassle in a live/AV setup, and the Balance Board needs it anyway.
- **No Python**: pybluez is unmaintained, and the native APIs (bthprops, BlueZ, IOBluetooth) are C/Objective-C anyway.
- **wiiuse is patched in place** rather than wrapped by a separate HID layer in wiimosc. Writing our own HID search would need wiiuse's internal handshake anyway, and closing/reopening all remotes on every new connection would interrupt the ones already connected.

## Other platforms (planned)

- **Linux**: wiiuse's direct L2CAP connection works without pairing. For permanent SYNC pairings, BlueZ's built-in `wiimote` plugin supplies the reversed-address PIN automatically, so pairing can be scripted over D-Bus (`org.bluez.Device1.Pair`, then `Trusted = true`) or `bluetoothctl`. The kernel's `hid-wiimote` driver may claim the device; this needs checking against wiiuse.
- **macOS**: pair with `IOBluetoothDevicePair` and answer `devicePairingPINCodeRequest` with the reversed host address, as [WiimotePairer](https://github.com/dolphin-emu/WiimotePairer) does. It may need a `bluetoothd` restart and the Bluetooth permission prompt (TCC).

## Status

| Item | State |
|---|---|
| Windows build (VS 2022, x64) | ✅ builds |
| Radio detection, stale removal, scanning | ✅ ran without errors (no remote nearby) |
| Pairing a Wiimote Plus (`RVL-CNT-01-TR`) via SYNC | ✅ works (callback-based PIN), wiiuse connects |
| Pairing via 1+2 | ⏳ untested |
| Pairing the Balance Board (`RVL-WBC-01`) via SYNC | ✅ works |
| Re-pairing an already paired remote put into SYNC mode again | ✅ works, wiiuse connects afterwards |
| Reconnect after power cycle (SYNC pairing) | ✅ Windows reconnects it on its own; HID device present afterwards (needs re-checking with the presence probe) |
| Tester: picking up devices already connected at startup (Wiimote Plus + Balance Board) | ✅ works, board identified |
| Tester: A presses on two Wiimotes | ✅ works |
| Tester: rumble / board LED blink, reconnect & disconnect reports, board button | ⏳ not yet confirmed |
| `--shutdown-on-exit` with the Balance Board | ✅ disconnects; stayed off in a second run |
| `--shutdown-on-exit` with Wiimotes | ✅ confirmed by the user |
| `--idle-timeout` (Wiimote Plus + board, via `wiimosc-stream`) | ✅ both shut down after the timeout |
| Linux / macOS backends | ❌ not implemented |
