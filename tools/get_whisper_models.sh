#!/usr/bin/env bash
# get_whisper_models.sh -- download Whisper speech-to-text models for the
# browser (Transformers.js / ONNX) from Hugging Face, without the Hugging Face
# command-line tool. Only what Transformers.js needs is fetched: the small
# config / tokenizer files and the two quantized ONNX files -- not the
# full-precision copies (hundreds of MB) the repos also hold.
#
#   tools/get_whisper_models.sh                         # base.en + small.en
#   tools/get_whisper_models.sh Xenova/whisper-base.en  # just one
#
# Environment:
#   OUT          download directory (default ./whisper-models); a zip of it is
#                made next to it (whisper-models.zip) unless NO_ZIP=1
#   HF_ENDPOINT  Hugging Face base URL (default https://huggingface.co)
#   HF_TOKEN     access token, only needed for gated / private repos
#
# Needs: bash, curl, and python3 or jq (to read the file list). Re-running
# skips files already complete and resumes partial ones. The large files are
# checked against the SHA-256 Hugging Face publishes for them.

set -euo pipefail

HF=${HF_ENDPOINT:-https://huggingface.co}
HF=${HF%/}
OUT=${OUT:-whisper-models}
MODELS=("$@")
[ ${#MODELS[@]} -eq 0 ] && MODELS=(Xenova/whisper-base.en Xenova/whisper-small.en)
WANT_ONNX="onnx/encoder_model_quantized.onnx onnx/decoder_model_merged_quantized.onnx"

AUTH=()
[ -n "${HF_TOKEN:-}" ] && AUTH=(-H "Authorization: Bearer $HF_TOKEN")
command -v curl >/dev/null || { echo "curl is required" >&2; exit 1; }

# The repo's file list as "name<TAB>size<TAB>sha256" lines (sha256 only for
# the large, LFS-stored files).
list_files() {
    local json
    json=$(curl -fsSL "${AUTH[@]}" "$HF/api/models/$1?blobs=true") || {
        echo "cannot read the file list of $1 from $HF (no network access, or a wrong name?)" >&2; return 1; }
    if command -v python3 >/dev/null; then
        printf '%s' "$json" | python3 -c '
import json, sys
for s in json.load(sys.stdin).get("siblings", []):
    lfs = s.get("lfs") or {}
    print("%s\t%s\t%s" % (s["rfilename"], s.get("size") or lfs.get("size") or "", lfs.get("sha256", "")))'
    elif command -v jq >/dev/null; then
        printf '%s' "$json" | jq -r '.siblings[] | [.rfilename, (.size // .lfs.size // ""), (.lfs.sha256 // "")] | @tsv'
    else
        echo "python3 or jq is required to read the file list" >&2; return 1
    fi
}

filesize() { stat -c %s "$1" 2>/dev/null || stat -f %z "$1"; }
sha256() { if command -v sha256sum >/dev/null; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi; }

total=0
for repo in "${MODELS[@]}"; do
    name=${repo##*/}
    echo "== $repo"
    files=$(list_files "$repo")
    # Top-level .json / .txt files, plus the two quantized ONNX files.
    want=$(printf '%s\n' "$files" | awk -F'\t' -v onnx="$WANT_ONNX" '
        BEGIN { n = split(onnx, o, " "); for (i = 1; i <= n; i++) need[o[i]] = 1 }
        ($1 !~ /\//  && $1 ~ /\.(json|txt)$/) || ($1 in need)')
    for f in $WANT_ONNX; do
        printf '%s\n' "$want" | cut -f1 | grep -qx "$f" || {
            echo "  $repo has no $f -- not a Transformers.js Whisper model?" >&2; exit 1; }
    done
    while IFS=$'\t' read -r path size sha; do
        [ -n "$path" ] || continue
        dest="$OUT/$name/$path"
        mkdir -p "$(dirname "$dest")"
        if [ -f "$dest" ] && [ -n "$size" ] && [ "$(filesize "$dest")" = "$size" ]; then
            printf '  %-45s %12s bytes  already here\n' "$path" "$size"
        else
            printf '  %-45s %12s bytes  ' "$path" "${size:-?}"
            # -C - resumes a partial .part file; --retry rides out short hiccups.
            if ! { [ -f "$dest.part" ] && [ -n "$size" ] && [ "$(filesize "$dest.part")" = "$size" ]; }; then
                curl -fL --retry 3 --retry-delay 2 -C - -s -S "${AUTH[@]}" \
                     -o "$dest.part" "$HF/$repo/resolve/main/$path"
            fi
            if [ -n "$size" ] && [ "$(filesize "$dest.part")" != "$size" ]; then
                echo "size mismatch (got $(filesize "$dest.part"))" >&2; exit 1
            fi
            mv "$dest.part" "$dest"
            echo "downloaded"
        fi
        if [ -n "$sha" ]; then
            if [ "$(sha256 "$dest")" != "$sha" ]; then
                echo "  $path: SHA-256 does not match Hugging Face's -- deleted, re-run to fetch again" >&2
                rm -f "$dest"; exit 1
            fi
            printf '  %-45s %12s        sha256 ok\n' "" ""
        fi
        total=$((total + $(filesize "$dest")))
    done <<< "$want"
done

echo "== $(awk -v b=$total 'BEGIN { printf "%.1f MB", b / 1e6 }') in $OUT/"
if [ "${NO_ZIP:-0}" != 1 ]; then
    rm -f "$OUT.zip" "$OUT.tar.gz"
    if command -v zip >/dev/null; then
        (cd "$(dirname "$OUT")" && zip -qr "$(basename "$OUT").zip" "$(basename "$OUT")") && echo "== $OUT.zip"
    else
        tar czf "$OUT.tar.gz" -C "$(dirname "$OUT")" "$(basename "$OUT")" && echo "== $OUT.tar.gz (no zip command found)"
    fi
fi
