#!/bin/bash
# Plays on a Wiimote's speaker while recording the Mac's microphone, then analyzes the recording (macOS only).
#
# usage: tools/speakermic/run.sh <name> <tone-hz> <seconds> [wiimosc-speaker options...]
#   e.g. tools/speakermic/run.sh adpcm1k 1000 8 --test-mode sine --frequency 1000
#
# Recordings and logs go to tools/speakermic/out/ (not versioned). The first run records 2 s of room noise there as
# the baseline. Environment: MIC (default "MacBook Pro Microphone", see `ffmpeg -f avfoundation -list_devices true
# -i ""`), SPEAKER (default build/wiimosc-speaker). Needs ffmpeg and python3 with numpy.
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$DIR/../.." && pwd)
OUT="$DIR/out"
MIC=${MIC:-MacBook Pro Microphone}
SPEAKER=${SPEAKER:-$ROOT/build/wiimosc-speaker}
NAME=$1; FREQ=$2; SECS=$3; shift 3
mkdir -p "$OUT"

record() { ffmpeg -hide_banner -loglevel error -y -f avfoundation -i ":$MIC" -t "$1" -ac 1 -ar 16000 "$2" 2>/dev/null; }

if [ ! -f "$OUT/silence.wav" ]; then
    echo "recording 2 s of room noise as the baseline (keep the remote quiet)"
    record 2 "$OUT/silence.wav"
fi

record $((SECS + 1)) "$OUT/$NAME.wav" &
REC=$!
"$SPEAKER" "$@" > "$OUT/$NAME.log" 2>&1 &
PID=$!
sleep "$SECS"
kill -INT $PID 2>/dev/null || true
wait $PID || true
wait $REC
grep -E "speaker on|speaker:" "$OUT/$NAME.log" | tail -2 || true
# The first ~3 s cover connecting and the speaker setup.
python3 "$DIR/analyze.py" "$OUT/$NAME.wav" --freq "$FREQ" --skip 3 --baseline "$OUT/silence.wav"
