#!/usr/bin/env python3
"""
iq_wav_to_cf32.py -- convert an SDR "IQ WAV" recording into the interleaved
float32 .cf32 baseband that tetrapol_bit_source (and the server) consume.

SDR capture tools (SDR#, SDRuno, HDSDR, GQRX, gqrx, rtl_sdr piped through sox,
and the samples on sigidwiki) commonly save IQ as a stereo WAV: the LEFT
channel is I, the RIGHT channel is Q, at the capture's sample rate and in
int16 / uint8 / int32 / float32 PCM. That is NOT what tetrapol_bit_source reads
(interleaved little-endian float32 I/Q at N*8000 Hz, one channel at baseband),
so this bridges the two -- and does the two things a raw WAV usually needs:

  * CHANNELIZE. A wideband capture has the TETRAPOL channel tuned off-center.
    --offset HZ frequency-translates that channel down to 0 Hz (the same
    freq_xlating step tetrapol-kit's own GNU Radio flowgraph does, default
    offset -267 kHz there). Give it the channel's frequency relative to the
    capture's center (positive = above center).
  * RESAMPLE. TETRAPOL is 8000 sym/s, so the demod wants N*8000 Hz. This
    resamples to --out-rate (default 16000 = 2 samples/symbol, exactly the
    reference flowgraph's decimated rate) with a rational polyphase filter.
    A 12.5 kHz channel low-pass (TETRAPOL channel width) is applied before
    decimation when channelizing, matching the reference.

Then run, e.g.:

    python3 tools/iq_wav_to_cf32.py capture.wav out.cf32 --offset -267000
    ./tetrapol_bit_source out.cf32 | tetrapol_dump

--swap-iq flips I and Q (spectral inversion): if a known-good capture won't
frame-lock in either bit polarity, the SDR may have stored I/Q swapped; this is
distinct from tetrapol_bit_source's --invert (which flips the demodulated bits).

Depends on numpy + scipy only.
"""

import argparse
import sys
import wave

import numpy as np


def read_iq_wav(path):
    """Return (iq_complex float64, sample_rate). LEFT=I, RIGHT=Q."""
    # Prefer scipy (handles int16/int32/uint8/float32 and odd chunks); fall
    # back to stdlib wave for plain PCM if scipy isn't importable.
    rate = None
    data = None
    try:
        from scipy.io import wavfile
        rate, data = wavfile.read(path)
    except Exception as e:  # noqa: BLE001 -- fall back on any scipy failure
        sys.stderr.write(f"iq_wav_to_cf32: scipy read failed ({e}); trying stdlib wave\n")
        with wave.open(path, "rb") as w:
            rate = w.getframerate()
            nch = w.getnchannels()
            sw = w.getsampwidth()
            raw = w.readframes(w.getnframes())
        dt = {1: np.uint8, 2: np.int16, 4: np.int32}.get(sw)
        if dt is None:
            raise SystemExit(f"iq_wav_to_cf32: unsupported sample width {sw} bytes")
        data = np.frombuffer(raw, dtype=dt).reshape(-1, nch)

    data = np.asarray(data)
    if data.ndim == 1:
        raise SystemExit(
            "iq_wav_to_cf32: mono WAV -- this is a single real channel, not IQ. "
            "An IQ recording is 2-channel (I=left, Q=right)."
        )
    if data.shape[1] < 2:
        raise SystemExit("iq_wav_to_cf32: need a 2-channel (I/Q) WAV")
    if data.shape[1] > 2:
        sys.stderr.write(
            f"iq_wav_to_cf32: {data.shape[1]} channels; using ch0=I, ch1=Q\n"
        )

    # Normalize to float in [-1, 1] by dtype full-scale. uint8 (RTL-SDR style)
    # is offset-binary centered on 128.
    dt = data.dtype
    if dt == np.uint8:
        i = data[:, 0].astype(np.float64) - 128.0
        q = data[:, 1].astype(np.float64) - 128.0
        scale = 128.0
    elif np.issubdtype(dt, np.integer):
        info = np.iinfo(dt)
        i = data[:, 0].astype(np.float64)
        q = data[:, 1].astype(np.float64)
        scale = float(max(abs(info.min), info.max))
    else:  # float32/float64 already ~[-1,1]
        i = data[:, 0].astype(np.float64)
        q = data[:, 1].astype(np.float64)
        scale = 1.0

    iq = (i + 1j * q) / scale
    return iq, int(rate)


def main():
    ap = argparse.ArgumentParser(description="Convert an IQ WAV to baseband .cf32 for tetrapol_bit_source")
    ap.add_argument("input", help="input IQ WAV (stereo: I=left, Q=right)")
    ap.add_argument("output", help="output interleaved float32 .cf32")
    ap.add_argument("--offset", type=float, default=0.0,
                    help="channel frequency relative to capture center, Hz "
                         "(positive = above center); shifted to 0 Hz. Default 0.")
    ap.add_argument("--out-rate", type=float, default=16000.0,
                    help="output sample rate, Hz -- must be N*8000 (default 16000 = 2 sps)")
    ap.add_argument("--low-pass", type=float, default=12500.0,
                    help="channel low-pass cutoff Hz applied when channelizing/decimating "
                         "(TETRAPOL channel width; default 12500)")
    ap.add_argument("--swap-iq", action="store_true", help="swap I and Q (spectral inversion)")
    args = ap.parse_args()

    from scipy import signal

    iq, fs = read_iq_wav(args.input)
    n_in = iq.size
    dur = n_in / fs if fs else 0.0
    sys.stderr.write(
        f"iq_wav_to_cf32: {args.input}: {n_in} IQ samples, {fs} Hz, {dur:.2f} s\n"
    )

    if args.swap_iq:
        iq = np.conj(iq)  # swapping I/Q == conjugation == spectral flip

    # 1) Frequency-translate the wanted channel to 0 Hz.
    if args.offset != 0.0:
        n = np.arange(n_in, dtype=np.float64)
        iq = iq * np.exp(-2j * np.pi * (args.offset / fs) * n)

    # 2) Channel low-pass before decimation (only meaningful when the input is
    #    wider than the channel, i.e. we're about to downsample a lot). resample
    #    adds its own anti-alias filter at the new Nyquist, but an explicit
    #    12.5 kHz channel filter on the wide input rejects strong adjacent
    #    TETRAPOL channels the reference flowgraph also filtered out.
    out_rate = args.out_rate
    if fs > out_rate * 1.5 and args.low_pass < fs / 2:
        taps = signal.firwin(129, args.low_pass, fs=fs, window="hann")
        iq = signal.lfilter(taps, 1.0, iq)

    # 3) Rational resample to out_rate.
    if abs(fs - out_rate) > 1e-6:
        g = np.gcd(int(round(fs)), int(round(out_rate)))
        up = int(round(out_rate)) // g
        down = int(round(fs)) // g
        # Keep the polyphase factors sane; if the ratio is awkward, prefilter
        # via an intermediate integer decimation first.
        if down > 20000:
            dec = max(1, int(fs // (out_rate * 4)))
            if dec > 1:
                iq = signal.decimate(iq, dec, ftype="fir", zero_phase=True)
                fs = fs / dec
                g = np.gcd(int(round(fs)), int(round(out_rate)))
                up = int(round(out_rate)) // g
                down = int(round(fs)) // g
        iq = signal.resample_poly(iq, up, down)

    n_out = iq.size
    sps = out_rate / 8000.0
    sys.stderr.write(
        f"iq_wav_to_cf32: -> {args.output}: {n_out} samples, {out_rate:.0f} Hz, "
        f"{sps:.3g} samples/symbol\n"
    )
    if abs(sps - round(sps)) > 1e-6:
        sys.stderr.write(
            f"iq_wav_to_cf32: WARNING out-rate {out_rate:.0f} is not a multiple of 8000; "
            f"pass --out-rate 16000 (2 sps) or 24000 (3 sps)\n"
        )

    # Interleave I,Q as little-endian float32.
    out = np.empty(n_out * 2, dtype="<f4")
    out[0::2] = iq.real.astype("<f4")
    out[1::2] = iq.imag.astype("<f4")
    out.tofile(args.output)


if __name__ == "__main__":
    main()
