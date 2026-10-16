# Speaker test tool

`wiimosc-speaker` pairs and connects devices like `wiimosc-pair` (see [Pairing](Pairing.md)), and plays audio on one Wiimote's speaker. It's a prototype for finding out what the speaker can do from a PC.

```
wiimosc-speaker --target-id 1 --test-mode sine
wiimosc-speaker --target-id 1 --test-mode file --file song.m4a
wiimosc-speaker --target-id 1 --test-mode trigger --file click.wav
```

| Option | Default | Meaning |
|---|---|---|
| `--target-id <id>` | `0` | Remote to play on. Other devices connect as usual, and are ignored. Combine with `--id` to make the target a specific remote. |
| `--test-mode` | `sine` | `sine`: a sine wave until Ctrl+C. `file`: plays `--file` once, then quits. `trigger`: plays `--file` from the start on every press of A (restarting it if it's still playing). `sweep`: the sine with each `--writer`, with 30 and 200 ms `--setup-delay` (6 variants of 6 s, announced), then quits. |
| `--file <path>` | | Audio file, anything Windows can decode (wav, mp3, m4a/AAC, wma, ...). Decoded and resampled up front, mixed down to mono, and normalized. |
| `--frequency <hz>` | `440` | Sine frequency |
| `--format adpcm\|pcm` | `adpcm` | 4-bit Yamaha ADPCM (40 samples per report) or 8-bit signed PCM (20 samples per report) |
| `--rate <hz>` | `2000` (ADPCM), `1000` (PCM) | Sample rate. Both defaults need 50 reports/s, see below. |
| `--volume <0-1>` | `1` | Speaker volume register, as a fraction of its maximum (`0x40` for ADPCM, `0xff` for PCM) |
| `--writer async\|wiiuse\|setreport` | `async` | How audio reports are sent (experiment, see "Debugging") |
| `--queue <1-16>` | `3` | Audio reports in flight at once with `async`. `1` waits for each write to complete. |
| `--setup-delay <ms>` | `30` | Pause after each speaker setup step |
| `--one-two`, `--no-auth`, `--id`, `--shutdown-on-exit`, `--idle-timeout` | | As for `wiimosc-pair` |

Options take their value as `--option value` or `--option=value`.

While playing, the tool prints a statistics line every 5 s and when a sound ends:

```
[1] speaker on: ADPCM, 2000 Hz, volume 64/127, 50.0 reports/s
[1] speaker: 5.0 s, 251 reports (50.2/s), write avg 0.1 ms / max 0.2 ms, max late 1.9 ms, 0 resyncs, 0 failed writes
```

- **write**: time spent sending a report, including waiting for a free queue entry. Near 0 means the link keeps up.
- **max late**: how much later than scheduled a report went out.
- **resyncs**: how often the sender fell behind by more than one report period and restarted its clock rather than bursting to catch up. Each one is an audible gap.

## Protocol

From [WiiBrew](https://wiibrew.org/wiki/Wiimote#Speaker), cross-checked against Dolphin's speaker emulation:

1. Report `0x14` with `0x04` turns the speaker on, and report `0x19` with `0x04` mutes it.
2. The speaker's registers (`0x04a20009`, `0x04a20001`, `0x04a20008`) get the values `0x01`, then `0x08`, then a 7-byte configuration, then `0x01`.
   - The configuration is `00 FF RR RR VV 00 00`.
   - `FF` is the format: `0x00` for ADPCM, `0x40` for 8-bit PCM.
   - `RRRR` is a little-endian divisor: the sample rate is 6 MHz / `RRRR` for ADPCM and 12 MHz / `RRRR` for PCM.
   - `VV` is the volume.
3. Report `0x19` with `0x00` unmutes.
4. Audio goes out as report `0x18`: a length byte (`length << 3`), then 20 bytes of samples. ADPCM packs two samples per byte, high nibble first.
5. On stop: mute, then speaker off.

Bit 0 of every output report is the rumble motor, so the tool copies wiiuse's rumble state into it. Register writes go through `wiiuse_write_data`, with 30 ms between them: they aren't acknowledged before it returns, and the remote drops writes that arrive too quickly. The ADPCM encoder is ffmpeg's `adpcm_yamaha`, with predictor 0 and step 127, reset whenever the speaker is set up again.

## Findings

All measured on the development machine: Windows 10, the Microsoft Bluetooth stack, with 2 Wiimote Plus and the Balance Board connected. Motion sensing was off, so only the board was streaming input.

- **A write to the remote takes ~15 ms to complete** (`WriteFile` on the HID handle). Waiting for each one caps the rate at ~60 reports/s.
- **The link carries ~61 audio reports/s at most.** Asynchronous writes (`--queue`) remove the waiting, but don't raise this ceiling: the driver still finishes one write every ~16 ms.

  | Rate | Reports/s needed | Result |
  |---|---|---|
  | ADPCM at 3000 Hz (WiiBrew's example rate) | 75 | ~60/s delivered, a resync every ~0.1 s: choppy |
  | ADPCM at 2600 Hz | 65 | ~61/s delivered, regular resyncs |
  | ADPCM at 2400 Hz | 60 | clean in one run, 2 resyncs in 9 s in another: right at the limit |
  | ADPCM at 2200 Hz | 55 | clean (writes 0.1 ms, max late 1.9 ms) |
  | ADPCM at 2000 Hz, PCM at 1000 Hz (defaults) | 50 | clean |

- The usable bandwidth is therefore ~1 kHz for ADPCM (Nyquist at 2000 Hz), or ~500 Hz for PCM. ADPCM is the better choice, even with its quantization noise. A 440 Hz sine is near the PCM limit at 1000 Hz.
- The ceiling may be specific to the stack or adapter, or may shrink with more devices streaming input; that hasn't been tested yet. With `--queue 1`, the write times show the driver's behaviour directly.
- `timeBeginPeriod(1)` is needed: with the default ~15 ms timer resolution, the 16–20 ms report period would jitter by a whole period.
- Decoding through Media Foundation works for wav and m4a (AAC), including paths with non-ASCII characters. A 3:48 song loads in ~2.3 s; resampling uses a tabulated windowed-sinc kernel, and computing it directly took ~9 s.
- Dropped reports would desync the ADPCM decoder from the encoder, causing a DC offset until the next setup. Clips get 0.1 s of silence appended, so the predictor returns to 0 between triggers.

## Debugging: no sound yet

The first version streamed with clean statistics, but the user heard nothing, on both remotes, in sine and trigger mode. Verified since, with the Debug build (wiiuse logs sent reports, and received ones when the read had to wait):

- All four register writes are acknowledged with error code 0 (`22 BB BB 16 00`), and their payloads are as intended (e.g. `04 a2 00 01 07 00 00 b8 0b 40 00 00`).
- After setup, a status report has the "speaker enabled" bit set. The tool now requests one and prints `[id] status: speaker enabled`.
- The remote acknowledges "speaker on" (`0x14`) only ~110 ms after it's sent, after the register writes had already gone through. The setup now waits 150 ms after `0x14`, in case the configuration was lost while the speaker powered up. **Not yet tested by ear.**
- Reading the speaker registers back (`0x04a20001`, 9 bytes) gives all zeros. Either they're write-only, or the configuration didn't stick; it's unknown which.

Second round, after the user reported a **faint hiss but no tone** in all of: default, `--volume 1`, `--rate 3000` (WiiBrew's exact ADPCM example config), and `--format pcm --rate 2000`. The hiss means the amplifier is on and unmuted, so either the configuration doesn't take effect, or the audio reports aren't accepted.

- WiiBrew and Dolphin match the setup sequence and report format exactly. Dolphin writes speaker data with plain `WriteFile` (report id + payload), as we do.
- The ADPCM volume range is `0x00–0x40` per WiiBrew (not `0x7f`); `--volume` is now scaled to that, and the default is `1` (`0x40`, WiiBrew's example value).
- New experiment options: `--writer async|wiiuse|setreport` (`setreport` = `HidD_SetOutputReport`, the HID control channel; ~37 reports/s at most), `--setup-delay <ms>`, and `--test-mode sweep`, which plays the sine for 6 s with each writer at 30 and 200 ms setup delay, announcing each variant. All six variants run and send without errors; whether any is audible is pending.

Still open: the audio reports themselves can't be seen in the log, since they bypass wiiuse. The rate divisor for 2000 Hz (`0x0bb8`) isn't one of WiiBrew's example values. A cross-check with Dolphin (real Wiimote, speaker enabled) would show whether the remotes and the Bluetooth stack can play sound at all.

## Implementation

- `src/speaker/WiimoteSpeaker`: speaker setup, encoding (ADPCM / PCM), and the paced sender thread with statistics. On Windows, audio reports bypass wiiuse and go through an `AsyncWriter` on the device handle (each write has its own `OVERLAPPED`; up to `--queue` in flight). Other reports use `wiiuse_send`, which isn't exported by `wiiuse.h` but is part of the static library.
- `AbstractSoundSource`: the sender pulls samples from it. Returning fewer samples than asked ends a sound, and returning 0 sends nothing.
- `src/speaker/Audio`: `resample` and `normalize` (portable), and `loadAudioFile` (Media Foundation: `IMFSourceReader`, decoded to float at the file's own rate and channels, then mixed and resampled in our code).
- `src/speaker/main.cpp`: CLI, `SineSource`, `ClipSource` (restartable, thread-safe), and `SpeakerTester`, the hub listener that runs a `WiimoteSpeaker` while the target is connected. The hub doesn't call `disconnected()` for devices still connected at shutdown, so `main` stops the speaker before `hub.stop()`.

## Status

| Item | State |
|---|---|
| Sine, ADPCM and PCM: speaker setup, paced streaming, statistics | ✅ runs without errors, timing as in the table above |
| File mode: wav, m4a, non-ASCII paths, quits at the end | ✅ runs |
| **Audible sound** | ❌ silent on two remotes in sine and trigger mode (first version), see "Debugging" below |
| Trigger mode (A press) | ⏳ not yet tested (needs a button press) |
| mp3 | ⏳ not tested; should work via Media Foundation |
| Disconnect and reconnect while playing | ⏳ not tested |
