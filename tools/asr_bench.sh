#!/usr/bin/env bash
# asr_bench.sh -- how fast and how readable is speech-to-text (whisper.cpp) on
# our decoded digital-voice audio? Run it on the machine that would do the
# transcribing, before building the explorer feature.
#
#   tools/asr_bench.sh [audio files or directories...]
#
#   With no arguments it uses the explorer's call recordings in
#   $DSD_NET_AUDIO_DIR (or ./net_audio). Any WAV (any rate, mono or stereo)
#   works; each is converted to the 16 kHz mono whisper.cpp needs.
#
# Environment:
#   MODELS     whisper models to try (default "tiny.en base.en small.en";
#              multilingual: tiny base small medium; add e.g. "small" for
#              non-English traffic)
#   THREADS    CPU threads (default: all cores)
#   WORK       working directory (default ./asr_bench): whisper.cpp, models
#              and resampled audio are kept there for re-runs
#   MAX_FILES  at most this many audio files (default 40)
#   WHISPER_EXTRA  extra whisper-cli options, e.g. "-ng" (force CPU) or
#              "-ac 512" -- a shorter audio context: Whisper always encodes a
#              30 s window, so a 3 s call costs about as much as a 30 s one;
#              -ac 512 (~10 s) makes short calls several times faster at some
#              cost in accuracy. Worth comparing: run once without, once with.
#
# Needs: git, cmake, a C++ compiler, python3, and network access to
# github.com (whisper.cpp) and huggingface.co (the models, 75 MB tiny.en,
# 142 MB base.en, 466 MB small.en). Prints, per model: each file's
# transcript and timing, the speed as a multiple of real time, how many
# seconds of audio per second it keeps up with, and what that means at a
# given call rate; plus what each model "hears" in silence and in noise
# (Whisper invents text there -- the explorer would have to filter it).

set -euo pipefail

MODELS=${MODELS:-"tiny.en base.en small.en"}
THREADS=${THREADS:-$(nproc 2>/dev/null || echo 4)}
WORK=${WORK:-$PWD/asr_bench}
MAX_FILES=${MAX_FILES:-40}
WHISPER_EXTRA=${WHISPER_EXTRA:-}
mkdir -p "$WORK/models" "$WORK/audio"

# ---- whisper.cpp -------------------------------------------------------------
WD="$WORK/whisper.cpp"
if [ ! -x "$WD/build/bin/whisper-cli" ]; then
    [ -d "$WD" ] || git clone --depth 1 https://github.com/ggml-org/whisper.cpp.git "$WD"
    echo "building whisper.cpp ($THREADS threads)..."
    cmake -S "$WD" -B "$WD/build" -DCMAKE_BUILD_TYPE=Release -DWHISPER_BUILD_TESTS=OFF > "$WORK/build.log"
    cmake --build "$WD/build" -j "$THREADS" --target whisper-cli >> "$WORK/build.log" 2>&1
fi
CLI="$WD/build/bin/whisper-cli"
echo "whisper.cpp $(git -C "$WD" log -1 --format=%h 2>/dev/null || echo '?') | $THREADS threads | $(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2 | sed 's/^ *//')"

# ---- models --------------------------------------------------------------------
for m in $MODELS; do
    f="$WORK/models/ggml-$m.bin"
    if [ ! -s "$f" ]; then
        echo "downloading model $m..."
        sh "$WD/models/download-ggml-model.sh" "$m" "$WORK/models" > /dev/null
    fi
done

# ---- audio: the calls, plus silence and noise ------------------------------------
INPUTS=("$@")
[ ${#INPUTS[@]} -eq 0 ] && INPUTS=("${DSD_NET_AUDIO_DIR:-./net_audio}")
python3 - "$WORK/audio" "$MAX_FILES" "${INPUTS[@]}" <<'PY'
# Convert every input WAV to 16 kHz mono 16-bit (windowed-sinc resampling,
# stdlib only), and add 3 s of silence and of low noise as controls.
import array, math, os, random, sys, wave
out, maxn, inputs = sys.argv[1], int(sys.argv[2]), sys.argv[3:]
files = []
for p in inputs:
    if os.path.isdir(p):
        files += sorted(os.path.join(p, f) for f in os.listdir(p) if f.lower().endswith('.wav'))
    elif os.path.isfile(p):
        files.append(p)
files = files[:maxn]
for f in os.listdir(out):
    os.remove(os.path.join(out, f))
def resample(x, rin, rout=16000, taps=32):
    if rin == rout: return x
    ratio = rout / rin; cutoff = min(1.0, ratio)
    n = int(len(x) * ratio); y = [0.0] * n
    for i in range(n):
        t = i / ratio; c = int(t); acc = 0.0
        for k in range(c - taps + 1, c + taps + 1):
            if 0 <= k < len(x):
                d = t - k
                w = 0.5 + 0.5 * math.cos(math.pi * d / taps)                    # Hann window
                s = cutoff * (1.0 if d == 0 else math.sin(math.pi * cutoff * d) / (math.pi * cutoff * d))
                acc += x[k] * s * w
        y[i] = acc
    return y
def write(path, y):
    a = array.array('h', (max(-32768, min(32767, int(round(v)))) for v in y))
    with wave.open(path, 'wb') as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(16000); w.writeframes(a.tobytes())
n = 0
for f in files:
    try:
        with wave.open(f) as w:
            ch, sw, rate, frames = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.readframes(w.getnframes())
    except Exception as e:
        print('  skipped %s: %s' % (f, e)); continue
    if sw != 2: print('  skipped %s: not 16-bit' % f); continue
    s = array.array('h', frames)
    x = [sum(s[i:i + ch]) / ch for i in range(0, len(s), ch)]
    if len(x) < rate * 0.2: continue
    write(os.path.join(out, '%03d_%s' % (n, os.path.basename(f))), resample(x, rate)); n += 1
random.seed(1)
write(os.path.join(out, 'zz_control_silence.wav'), [0.0] * 48000)
write(os.path.join(out, 'zz_control_noise.wav'), [random.gauss(0, 300) for _ in range(48000)])
print('%d audio files (+ 2 controls) -> %s' % (n, out))
PY

# ---- run ------------------------------------------------------------------------
dur() { python3 -c "import wave,sys; w=wave.open(sys.argv[1]); print('%.2f' % (w.getnframes()/w.getframerate()))" "$1"; }
now() { date +%s.%N; }
SUMMARY=()
for m in $MODELS; do
    model="$WORK/models/ggml-$m.bin"
    lang=en; [[ "$m" == *.en ]] || lang=auto
    echo; echo "================ $m ================"
    # load once, untimed (warms the page cache)
    "$CLI" -m "$model" -f "$WORK/audio/zz_control_silence.wav" -t "$THREADS" -l "$lang" -nt $WHISPER_EXTRA > /dev/null 2>&1 || true
    tot_a=0; tot_t=0; ncalls=0
    for f in "$WORK"/audio/*.wav; do
        a=$(dur "$f")
        # whisper.cpp reports its own timings on stderr: the recognition time
        # is total minus model load (a server keeps the model loaded).
        txt=$("$CLI" -m "$model" -f "$f" -t "$THREADS" -l "$lang" -nt $WHISPER_EXTRA 2>"$WORK/stderr.txt" | tr -s ' \n' ' ' | sed 's/^ *//;s/ *$//') || true
        t=$(python3 - "$WORK/stderr.txt" <<'PY'
import re, sys
s = open(sys.argv[1], errors='replace').read()
g = lambda k: float(re.search(k + r' time =\s*([0-9.]+) ms', s).group(1)) if re.search(k + r' time =\s*([0-9.]+) ms', s) else None
tot, load = g('total'), g('load')
print('%.2f' % ((tot - (load or 0)) / 1000) if tot is not None else 'ERR')
PY
)
        if [ "$t" = ERR ]; then echo "$(basename "$f"): whisper-cli failed:"; tail -3 "$WORK/stderr.txt"; continue; fi
        printf '%-48s %6ss audio %6ss  x%-5s | %s\n' "$(basename "$f")" "$a" "$t" \
            "$(python3 -c "print('%.1f' % ($a / max($t, 1e-3)))")" "${txt:-<nothing>}"
        case "$f" in *zz_control*) ;; *) tot_a=$(python3 -c "print($tot_a + $a)"); tot_t=$(python3 -c "print($tot_t + $t)"); ncalls=$((ncalls + 1));; esac
    done
    SUMMARY+=("$(python3 -c "
a, t, n = $tot_a, $tot_t, max($ncalls, 1)
per = t / n
print('%-10s %3d files, %6.1fs audio in %6.1fs (x%.1f real time) | %.2f s per file -> one transcriber keeps up with ~%.1f calls/s' % ('$m', n, a, t, a / max(t, 1e-3), per, 1 / max(per, 1e-3)))")")
done
echo; echo "================ summary ($THREADS threads; recognition time with the model loaded, as a server would run) ================"
echo "(Whisper's cost is roughly per call -- it encodes a fixed 30 s window -- so 'calls/s' is the number to compare with the explorer's Calls / s.)"
printf '%s\n' "${SUMMARY[@]}"
