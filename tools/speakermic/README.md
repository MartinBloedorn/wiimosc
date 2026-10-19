# Speaker microphone tests

Scripts that measure what a Wiimote's speaker plays, through the Mac's built-in microphone (remote ~10 cm away). They're how the findings in [doc/Speaker.md](../../doc/Speaker.md) ("Microphone measurements") were made. macOS only (ffmpeg's `avfoundation` input); needs ffmpeg and python3 with numpy.

| Script | Purpose |
|---|---|
| `run.sh <name> <tone-hz> <seconds> [speaker options]` | Records while `wiimosc-speaker` plays, then runs `analyze.py`. Output in `out/` (not versioned). |
| `analyze.py <wav> [--freq] [--skip] [--baseline]` | Level, band levels against the room-noise baseline, top peaks, the tone's share of the energy, and the 50 ms envelope. |
| `bursts.py <wav>` | Isolated bursts: spacing, duration and dominant frequency. Measures the speaker's actual rate with few reports per second. |
| `envelope.py <wav>` | Modulation frequency of the envelope (how often the sound chops) and a 100 ms sketch. |
| `segments.py <wav> <tone-hz>...` | Level, peak and tone shares per 250 ms, for checking a file's pitch and timing. |

Examples:

```
# Tone at the defaults (ADPCM, 6000 Hz)
tools/speakermic/run.sh default 1000 8

# The speaker's actual rate: 10 reports/s with a 4-sample sine period; burst pitch x 4 = samples/s
tools/speakermic/run.sh rate_pcm 50 9 --format pcm --rate 200 --frequency 50 --volume 0.25
python3 tools/speakermic/bursts.py tools/speakermic/out/rate_pcm.wav
```
