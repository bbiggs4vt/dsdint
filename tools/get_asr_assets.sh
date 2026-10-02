#!/usr/bin/env bash
# get_asr_assets.sh -- fetch what the network explorer's speech-to-text
# ("Transcribe on play") needs, into the folder dsd-server serves it from, so
# browsers load it from the server instead of the internet:
#
#   transformers.min.js                     Transformers.js (runs Whisper in the browser)
#   ort/ort-wasm-simd-threaded.asyncify.*   its ONNX Runtime WebAssembly
#   models/Xenova/whisper-base/...          the Whisper model (quantized ONNX)
#
#   tools/get_asr_assets.sh                  # base (the default)
#   tools/get_asr_assets.sh base small       # base, and small (slower, more accurate)
#   tools/get_asr_assets.sh base.en          # an English-only model (Hugging Face only)
#
# Models: tiny, base, small (multilingual -- the explorer's Language setting
# picks the language) or tiny.en, base.en, small.en (English only).
#
# Environment:
#   DEST          where to put it: DSD_NET_ASR_DIR if set, else ./net_asr --
#                 the server's default when it runs from this directory with
#                 DSD_NET_LOG_DIR / DSD_IQ_LOG_DIR unset (else net_asr/ in
#                 that directory). Copy the folder there if you fetch it on
#                 another machine.
#   MODEL_SOURCE  hf (Hugging Face, Xenova's repos; the default, falling back to
#                 npm), or npm (a mirror of the same files: sts-whisper-*)
#
# Needs: bash, curl, tar, python3; network access to registry.npmjs.org (the
# library and the npm models) and/or huggingface.co. Re-running skips what is
# already there. Package tarballs are checked against the registry's SHA-512;
# Hugging Face files against Hugging Face's SHA-256 (get_whisper_models.sh).

set -euo pipefail

TRANSFORMERS_VERSION=4.3.0
# The ONNX Runtime build Transformers.js $TRANSFORMERS_VERSION was made with
# (its package.json): the .mjs / .wasm must match the bundled JavaScript.
ORT_VERSION=1.31.0-dev.20260914-8d85527a0
NPM=${NPM_REGISTRY:-https://registry.npmjs.org}
NPM=${NPM%/}

DEST=${DEST:-${DSD_NET_ASR_DIR:-net_asr}}
SOURCE=${MODEL_SOURCE:-hf}
MODELS=("$@")
[ ${#MODELS[@]} -eq 0 ] && MODELS=(base)
HERE=$(cd "$(dirname "$0")" && pwd)
for t in curl tar python3; do command -v $t >/dev/null || { echo "$t is required" >&2; exit 1; }; done
mkdir -p "$DEST/ort" "$DEST/models/Xenova"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# Download npm package $1@$2 into $TMP/$1.tgz, checked against the registry's
# SHA-512 ("dist.integrity"); prints the file's path.
npm_fetch() {
    local pkg=$1 ver=$2 meta url integrity got
    meta=$(curl -fsSL "$NPM/$pkg/$ver") || { echo "cannot reach $NPM for $pkg@$ver" >&2; return 1; }
    read -r url integrity < <(printf '%s' "$meta" | python3 -c '
import json, sys
d = json.load(sys.stdin)["dist"]
print(d["tarball"], d.get("integrity", ""))')
    local out="$TMP/${pkg//\//_}.tgz"
    echo "  $pkg@$ver: downloading $(basename "$url")" >&2
    curl -fL --retry 3 -s -S -o "$out" "$url"
    if [ -n "$integrity" ]; then
        got="sha512-$(python3 -c 'import base64, hashlib, sys; print(base64.b64encode(hashlib.sha512(open(sys.argv[1], "rb").read()).digest()).decode())' "$out")"
        [ "$got" = "$integrity" ] || { echo "  $pkg@$ver: SHA-512 does not match the registry's" >&2; return 1; }
    fi
    printf '%s' "$out"
}

echo "== Transformers.js $TRANSFORMERS_VERSION -> $DEST/"
if [ -s "$DEST/transformers.min.js" ] && [ -s "$DEST/ort/ort-wasm-simd-threaded.asyncify.wasm" ] &&
   [ "$(cat "$DEST/VERSION" 2>/dev/null)" = "transformers $TRANSFORMERS_VERSION onnxruntime-web $ORT_VERSION" ]; then
    echo "  already here"
else
    tgz=$(npm_fetch @huggingface/transformers "$TRANSFORMERS_VERSION")
    tar xzf "$tgz" -C "$TMP" package/dist/transformers.min.js
    mv "$TMP/package/dist/transformers.min.js" "$DEST/transformers.min.js"
    rm -rf "$TMP/package"
    tgz=$(npm_fetch onnxruntime-web "$ORT_VERSION")
    tar xzf "$tgz" -C "$TMP" package/dist/ort-wasm-simd-threaded.asyncify.mjs package/dist/ort-wasm-simd-threaded.asyncify.wasm
    mv "$TMP"/package/dist/ort-wasm-simd-threaded.asyncify.* "$DEST/ort/"
    rm -rf "$TMP/package" "$TMP"/*.tgz
    echo "transformers $TRANSFORMERS_VERSION onnxruntime-web $ORT_VERSION" > "$DEST/VERSION"
fi

for m in "${MODELS[@]}"; do
    m=${m#Xenova/}; m=${m#whisper-}
    case "$m" in tiny|base|small|tiny.en|base.en|small.en) ;; *) echo "unknown model '$m' (tiny, base, small, tiny.en, base.en, small.en)" >&2; exit 1;; esac
    name=whisper-$m dir="$DEST/models/Xenova/whisper-$m"
    echo "== $name -> $dir/"
    if [ -s "$dir/onnx/encoder_model_quantized.onnx" ] && [ -s "$dir/onnx/decoder_model_merged_quantized.onnx" ] &&
       [ -s "$dir/tokenizer.json" ]; then
        echo "  already here"; continue
    fi
    ok=0
    if [ "$SOURCE" = hf ]; then
        if OUT="$DEST/models/Xenova" NO_ZIP=1 bash "$HERE/get_whisper_models.sh" "Xenova/$name" | sed 's/^/  /'; then ok=1
        else echo "  Hugging Face failed; trying the npm mirror" >&2; fi
    fi
    if [ $ok = 0 ]; then
        case "$m" in *.en) echo "  $name is only on Hugging Face (npm has tiny, base and small)" >&2; exit 1;; esac
        tgz=$(npm_fetch "sts-whisper-$m" 1.0.0)
        tar xzf "$tgz" -C "$TMP" "package/models/Xenova/$name"
        rm -rf "$dir"
        mv "$TMP/package/models/Xenova/$name" "$dir"
        rm -rf "$TMP/package" "$tgz"
    fi
done

echo "== done: $(du -sh "$DEST" | cut -f1) in $DEST/"
echo "   The explorer finds it when dsd-server's DSD_NET_ASR_DIR points here (or it is net_asr/"
echo "   in the server's log directory); reload the explorer page."
