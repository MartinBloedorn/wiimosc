#!/bin/bash
# Plays on a Wiimote's speaker while recording the microphone, then analyzes the recording (macOS, Windows/Git Bash).
#
# usage: tools/speakermic/run.sh <name> <tone-hz> <seconds> [wiimosc-speaker options...]
#   e.g. tools/speakermic/run.sh adpcm1k 1000 8 --test-mode sine --frequency 1000
#
# Recordings and logs go to tools/speakermic/out/ (not versioned). The first run records 2 s of room noise there as
# the baseline. Environment: MIC (default "MacBook Pro Microphone" / "Microphone Array (Realtek High Definition
# Audio)", see `ffmpeg -f avfoundation -list_devices true -i ""` or `ffmpeg -list_devices true -f dshow -i dummy`),
# SPEAKER (default build/wiimosc-speaker or build/Release/wiimosc-speaker.exe), PYTHON (default python3 / py).
# Needs ffmpeg and python with numpy.
#
# On Windows, Git Bash can't send Ctrl+C to the speaker tool, so without speaker options the script plays a tone file
# (tone.py) with --test-mode file, which quits by itself. Pass --test-mode file with your own --file otherwise.
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$DIR/../.." && pwd)
OUT="$DIR/out"
NAME=$1; FREQ=$2; SECS=$3; shift 3
mkdir -p "$OUT"

case "$(uname -s)" in
MINGW*|MSYS*|CYGWIN*)
    IS_WINDOWS=1
    MIC=${MIC:-Microphone Array (Realtek High Definition Audio)}
    SPEAKER=${SPEAKER:-$ROOT/build/Release/wiimosc-speaker.exe}
    PYTHON=${PYTHON:-py}
    record() { ffmpeg -hide_banner -loglevel error -y -f dshow -i "audio=$MIC" -t "$1" -ac 1 -ar 16000 "$2" 2>/dev/null; }
    ;;
*)
    IS_WINDOWS=
    MIC=${MIC:-MacBook Pro Microphone}
    SPEAKER=${SPEAKER:-$ROOT/build/wiimosc-speaker}
    PYTHON=${PYTHON:-python3}
    record() { ffmpeg -hide_banner -loglevel error -y -f avfoundation -i ":$MIC" -t "$1" -ac 1 -ar 16000 "$2" 2>/dev/null; }
    ;;
esac

if [ ! -f "$OUT/silence.wav" ]; then
    echo "recording 2 s of room noise as the baseline (keep the remote quiet)"
    record 2 "$OUT/silence.wav"
fi

if [ -n "$IS_WINDOWS" ] && ! printf '%s\n' "$@" | grep -q -- "--test-mode"; then
    "$PYTHON" "$DIR/tone.py" "$OUT/$NAME-tone.wav" "$FREQ" "$SECS" --lead 1
    set -- "$@" --test-mode file --file "$(cygpath -w "$OUT/$NAME-tone.wav")"
fi

echo "playing; connect the target remote now (press a button, or SYNC if it isn't paired)"
"$SPEAKER" "$@" > "$OUT/$NAME.log" 2>&1 &
PID=$!
# Record from when the speaker comes on, so the recording doesn't depend on how long connecting takes.
until grep -q "speaker on" "$OUT/$NAME.log" 2>/dev/null || ! kill -0 $PID 2>/dev/null; do sleep 0.2; done
record $((SECS + 1)) "$OUT/$NAME.wav" &
REC=$!
if [ -n "$IS_WINDOWS" ]; then
    wait $PID || true
else
    sleep "$SECS"
    kill -INT $PID 2>/dev/null || true
    wait $PID || true
fi
wait $REC
grep -E "speaker on|speaker:" "$OUT/$NAME.log" | tail -2 || true
"$PYTHON" "$DIR/analyze.py" "$OUT/$NAME.wav" --freq "$FREQ" --skip 1 --baseline "$OUT/silence.wav"
