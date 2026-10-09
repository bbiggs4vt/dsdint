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

## Backend diagnostics vs. user-facing verdict

The user sees a single "audio quality bad" badge (no cause). The **backend**
may log richer per-call diagnostics for field debugging — but only the
honest, non-inference discriminators:

- **FEC / link error rate** — identifies degraded SNR (high correction counts,
  intermittent sync). Direct measurement.
- **Acoustic quality score** — the Layer 2 verdict.
- **Signalled-crypto metadata (key id / algorithm id)** — for signalled
  systems (Motorola EP PI header, P25 ESS, DMR privacy indicator) the
  protocol *announces* encryption and dsd-fme already parses the key id/algo
  (it is what the Keys tab is built on). This is reading the protocol, not
  inference, so recording it is fine.

Those three separate the cases a field debugger cares about:
- high error rate -> RF problem,
- signalled key id -> encrypted (protocol said so),
- clean link + no signalling + structureless audio -> "unusable, cause
  undetermined" (the honest answer).

### Explicitly NOT built: unsignalled-encryption inference

Do NOT build a classifier that *infers* "encrypted" from "clean link +
structureless audio" to flag unsignalled/covert encryption (e.g. TYT EP).
Two reasons:

1. **Unreliable.** Clean-link-but-structureless audio is also a bad decode,
   wrong protocol params, or a vocoder edge case — real false-positive modes.
   It is a *worse* debugging signal than error-rate + signalling, not better.
2. **It is the piece that generalizes into surveillance.** A general
   unsignalled-encryption detector is an interception capability on any
   network regardless of intent, and (paired with forced-decrypt) is the
   thing already off the table.

For one's own network it buys little anyway: signalled crypto is already
announced, and the operator already knows which of their own TGs/radios run
unsignalled EP by their IDs (they programmed them), so "clean link + bad
audio on TG X" is already explained by "TG X is our EP talkgroup."

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

## Calibration

Thresholds need a **gradient** of signal conditions, not just
clear-vs-garbled. The cheap, repeatable way to get that gradient is to
**synthesize it from a clean capture by injecting AWGN**, rather than
collecting field captures.

### AWGN synthesis (primary approach)

We already log raw baseband IQ (BLUE/CF files from `open_iq_log`). Take one
clean call's IQ, add **complex additive white Gaussian noise** (independent
Gaussian on I and Q) at a swept set of levels, and replay each degraded
version through the same FM demod + dsd-fme. As noise rises, the decoder's
FEC correction counts climb monotonically to total failure — a controlled
gradient on demand.

What it calibrates, and the limits:

- **Layer 1 (RF/link quality): ideal use.** No absolute SNR figure needed —
  sweep the noise amplitude and let the *measured error rate itself* be the
  calibrated axis for good/marginal/unusable.
- **Noise goes in the IQ/RF domain, never the PCM.** Noise added to decoded
  audio only degrades it cosmetically and never exercises the FEC; bit errors
  only arise upstream of the demod/symbol slicer.
- **Bandwidth / processing gain.** Captures are wideband (e.g. 2 MHz) with
  the signal in a 12.5 kHz channel. The physically correct model is wideband
  AWGN that the channel filter then narrows, so "IQ SNR" != "in-channel SNR"
  by the bandwidth ratio. Simplest honest approach: sweep empirically and map
  to *measured error rate*, not an absolute dB number.
- **AWGN is not fading.** This synthesizes the *static weak-signal* gradient.
  Real mobile fading/flutter produces *burst* errors that differ from AWGN's
  uniform errors — a gap to note, not cover, in the first pass.
- **AWGN does NOT synthesize the encrypted case.** AWGN gives
  structureless-*from-noise*; encryption gives structureless-*from-a-clean-
  link*. They likely land in a similar region of the Layer 2 acoustic feature
  space (both lack speech structure), so a Layer 2 metric tuned on
  AWGN-garble will very probably also flag encrypted — but to *confirm* that,
  use one real known-encrypted strong-signal capture (the TYT EP ones already
  exist). Stays within the guardrail: confirming the quality metric flags
  unusable audio, not building an encryption detector.

### Plumbing needed for AWGN synthesis

No turnkey IQ replay exists: `assoc_replay.hpp` replays the event log, not IQ.
But `FmDemod::process(const cf32* in, ...)` can be driven straight from a
buffer, and BLUE/CF IQ read/write already exists (`blue_writer.hpp`,
`tools/make_test_bluefile.py`). So build a small standalone calibration
harness tool: read capture -> add complex AWGN at a swept level -> drive
`FmDemod::process` -> dsd-fme -> record error rate + acoustic features per
level.

### Still worth one real capture each

- One real known-encrypted strong-signal call (confirm Layer 2 flags it).
- Optionally one real fading/dropping-out call (the gap AWGN leaves).

## Open questions to settle before building

1. Scope: per-call badge only, or also network/entity roll-up?
2. Build with provisional (uncalibrated, clearly labelled) thresholds now and
   tune later, or wait for the gradient captures first?
3. Layer 2 feature set: start with spectral flatness alone (simplest, catches
   noise + encrypted), or include voiced-ratio / energy-modulation from the
   outset?
