#!/usr/bin/env bash
# Generate the audio fixtures used by tests/test_session_pager.cpp.
#
# Usage: tools/make_pager_fixtures.sh <multimon-ng source dir, already built> <out dir>
#
# Needs: <src>/build/gen-ng (built alongside multimon-ng) and sox (to convert
# the bundled FLAC captures). Every fixture is raw mono s16le at 22050 Hz --
# i.e. discriminator audio; the e2e test FM-modulates it into IQ.
#
# Two kinds of fixture:
#   gen_*   synthetic pages from multimon-ng's own encoder (gen-ng), with
#           known capcode + text, covering POCSAG 512/1200/2400 alpha/numeric
#           and FLEX alphanumeric;
#   real_*  multimon-ng's bundled off-air recordings (POCSAG 512/1200/2400
#           and a 1600 bps 2-FSK FLEX capture from the Dutch P2000 network).
set -euo pipefail

src=${1:?multimon-ng source dir}
out=${2:?output dir}
gen="$src/build/gen-ng"
samples="$src/test/samples"
mkdir -p "$out"

"$gen" -t raw -P "HELLO PAGER 123" -A 424242 -B 1200 "$out/gen_pocsag1200_alpha.raw" >/dev/null 2>&1
"$gen" -t raw -P "5551234" -N -A 1000 -B 512 "$out/gen_pocsag512_numeric.raw" >/dev/null 2>&1
"$gen" -t raw -P "FAST 2400 BAUD PAGE" -A 2097151 -B 2400 "$out/gen_pocsag2400_alpha.raw" >/dev/null 2>&1
"$gen" -t raw -f "FLEX TEST MSG" -F 1122334 "$out/gen_flex_alpha.raw" >/dev/null 2>&1

to_raw() { sox "$1" -t raw -e signed -b 16 -r 22050 -c 1 "$2"; }
to_raw "$samples/POCSAG_sample_-_512_bps.flac"  "$out/real_pocsag512.raw"
to_raw "$samples/POCSAG_sample_-_1200_bps.flac" "$out/real_pocsag1200.raw"
to_raw "$samples/POCSAG_sample_-_2400_bps.flac" "$out/real_pocsag2400.raw"
to_raw "$samples/FLEX_1600_2fsk_P2000_proef_alarm.flac" "$out/real_flex1600.raw"

ls -l "$out"
