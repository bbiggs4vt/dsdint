# Audio quality / intelligibility check (design, parked)

Status: **not built** — design parked on branch `claude/audio-quality-check`.

## Goal

Flag calls whose audio is **not usable** so a listener knows before they
bother listening, and so the explorer can mark/filter them. "Not usable"
means any of: a weak/noisy RF link, a bad decode, or audio that simply does
not resemble intelligible speech.

This is a **signal-quality indicator, not an encryption detector.** It
reports *whether* the audio is usable, never *why*. See "Guardrail" below.

## The three failure modes, and what catches each

| Failure mode | FEC / bit-error layer | Acoustic / intelligibility layer |
|---|---|---|
| Weak / fading / noisy RF | catches | catches |
| Clean RF but bad decode (garble) | sometimes | catches |
| Clean RF, **encrypted** | **misses** | catches |

The important, non-obvious point:

**The decoder's FEC error rate does NOT catch encrypted audio.**
Encryption happens *above* the error-correction layer. On a strong-signal
encrypted call the bits arrive intact, so the FEC (DMR Golay/Hamming/BPTC,
and the P25/NXDN equivalents) passes with near-zero corrections — the link
looks clean. The AMBE codewords are valid; they just decode to noise
because the payload was scrambled before transmission. So an FEC-only metric
gives an encrypted call a clean bill of health.

To catch "all cases where audio may not be usable" — which explicitly
includes encrypted — a second layer is required that looks at the decoded
audio itself.

## Two-layer approach

### Layer 1 — RF/link quality (decoder FEC error rate)

Source: dsd-fme already computes per-voice-superframe error-correction counts
as it decodes. That is a direct measurement of how many bits the RF link
corrupted — a real SNR/link-quality number, not a heuristic.

- Catches: weak / noisy / fading RF.
- Misses: strong-signal encrypted audio (bits are clean).
- Why preferred over a derived heuristic for the RF case: it is the decoder's
  own measurement, defensible, and not conflated with encryption.

Plumbing notes:
- dsd-fme prints this inline at higher verbosity, but the current parser
  (`classify_dsd_fme_line` in `src/dsd_process.cpp`) drops those lines as
  `unknown`. Need a **real verbose capture** of a known-good call AND a
  known-weak (fading/marginal) call to pin the exact log-line strings — do
  not guess the format from memory.
- Accumulate per call: mean / peak correction rate, % unrecoverable frames.

### Layer 2 — acoustic intelligibility (decoded PCM)

Source: the decoded PCM we already relay over UDP. Measure whether the audio
*sounds like speech*:
- spectral flatness / entropy (noise and encrypted output are spectrally
  flat / near-white; speech is structured),
- voiced-frame ratio / presence of formant structure,
- long-term energy modulation (speech has syllabic rhythm; noise does not).

- Catches: noise, garble, dead air, AND encrypted-sounds-like-noise.
- Reports a single verdict with **no cause attribution**.

An AMBE pitch-field heuristic (silence/erasure/tone-frame ratios from the
vocoder's b0 pitch code) is a *possible* alternative source for Layer 2, but:
- it was derived from only 4 calls (clear-vs-garbled, not a gradient),
- it conflates bad-RF garble with encrypted garble,
- it requires feeding raw AMBE frames out of dsd-fme, which the live pipeline
  does not currently ingest.
Prefer the acoustic measure on the PCM we already have. Revisit pitch-field
only if the acoustic measure proves insufficient.

## Guardrail (important)

Layer 2 **will** fire on encrypted audio. That is intended and useful — an
encrypted call genuinely is not usable to the listener. But:

- It must report **"unusable / unintelligible"**, never **"encrypted."**
- It must NOT distinguish cause, must NOT act as an encryption-presence
  oracle, and must NOT feed any forced-key / forced-decrypt path.
- Under a quality banner that treats encrypted audio the same as noise, this
  is honest and serves the stated goal (know when audio is not worth
  listening to). The thing deliberately NOT built is a detector whose output
  is "this call is encrypted: yes/no."

## Surfacing

- Per-call quality badge in the explorer: **good / marginal / unusable**,
  with the raw numbers (error rate, spectral flatness) on hover.
- Open question: also roll up to network/entity level (e.g. "this talkgroup
  is consistently marginal")?
- Purely additive; touches nothing in the key/BP paths.

## Calibration — the real blocker

Thresholds need real captures across a **gradient** of conditions, not just
clear-vs-garbled:
- one clean / full-quieting call,
- one marginal call,
- one actively dropping-out call,
- (for Layer 2) one known-encrypted strong-signal call.

Without the gradient the badge is a guess with a confident color on it.
Captures should include dsd-fme's verbose logs plus the usual IQ/audio
capture so Layer 1's log format and Layer 2's PCM features can both be
calibrated in one pass.

## Open questions to settle before building

1. Scope: per-call badge only, or also network/entity roll-up?
2. Build with provisional (uncalibrated, clearly labelled) thresholds now and
   tune later, or wait for the gradient captures first?
3. Layer 2 feature set: start with spectral flatness alone (simplest, catches
   noise + encrypted), or include voiced-ratio / energy-modulation from the
   outset?
