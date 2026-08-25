#!/usr/bin/env python3
"""Build a test BLUE IQ file from a 48 kHz discriminator capture.

Takes S16LE 48 kHz mono discriminator audio (e.g. DSDcc's
samples/dmr_it_8.dis), FM-modulates it into complex IQ at a chosen
sample rate, and writes an attached-header MIDAS BLUE file (type 1000,
format CI, little-endian) that tools/midas_ws_client.py and
tools/stream_load_test.py can stream at the server.

The modulation exactly inverts FmDemodulator's discriminator at the
default gain (26000), the same construction the repo's verified
full-stack tests use -- so the resulting file decodes sample-exactly
through the server. Rates below 48000 resample the discriminator first
(the DMR signal fits comfortably; 32000 is verified end to end).

    python3 tools/make_test_bluefile.py dmr_it_8.dis dmr_32k.tmp --rate 32000

Stdlib only.
"""

import argparse
import array
import math
import struct
import sys


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("dis", help="input: S16LE 48 kHz mono discriminator capture")
    ap.add_argument("out", help="output: BLUE file (type 1000, format CI)")
    ap.add_argument("--rate", type=float, default=32000.0,
                    help="IQ sample rate of the output (default 32000)")
    ap.add_argument("--gain", type=float, default=26000.0,
                    help="discriminator gain convention to invert (default 26000)")
    ap.add_argument("--amplitude", type=float, default=0.8,
                    help="IQ amplitude as a fraction of int16 full scale (default 0.8)")
    args = ap.parse_args()

    disc = array.array("h")
    with open(args.dis, "rb") as f:
        disc.frombytes(f.read())
    if sys.byteorder == "big":
        disc.byteswap()

    step = 48000.0 / args.rate  # input samples per output sample
    scale = args.amplitude * 32767.0
    ci = array.array("h")
    phase, pos = 0.0, 0.0
    n = len(disc)
    while pos + 1 < n:
        i0 = int(pos)
        frac = pos - i0
        d = disc[i0] + frac * (disc[i0 + 1] - disc[i0])
        pos += step
        # d is radians*gain referenced to a 48 kHz sample interval; at the
        # output rate each sample spans `step` input intervals.
        phase += d * step / args.gain
        if phase > math.pi:
            phase -= 2.0 * math.pi
        elif phase < -math.pi:
            phase += 2.0 * math.pi
        ci.append(int(round(math.cos(phase) * scale)))
        ci.append(int(round(math.sin(phase) * scale)))
    if sys.byteorder == "big":
        ci.byteswap()
    payload = ci.tobytes()

    hdr = bytearray(512)
    hdr[0:4] = b"BLUE"
    hdr[4:8] = b"EEEI"
    hdr[8:12] = b"EEEI"
    struct.pack_into("<i", hdr, 12, 0)                    # attached
    struct.pack_into("<d", hdr, 32, 512.0)                # data_start
    struct.pack_into("<d", hdr, 40, float(len(payload)))  # data_size
    struct.pack_into("<i", hdr, 48, 1000)                 # type
    hdr[52:54] = b"CI"
    struct.pack_into("<d", hdr, 256, 0.0)                 # xstart
    struct.pack_into("<d", hdr, 264, 1.0 / args.rate)     # xdelta

    with open(args.out, "wb") as f:
        f.write(bytes(hdr))
        f.write(payload)
    print(f"{args.out}: {len(payload) // 4} complex samples at {args.rate:.0f} Hz "
          f"({len(payload) / 4 / args.rate:.1f} s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
