# Audio quality / intelligibility check

Status: **Layer 2 built** on branch `claude/audio-quality-check`. Layer 1
(FEC error rate) and threshold calibration are still to do.

## Implementation status

- **Abandoned — spectral flatness on the PCM.** The first attempt measured
  spectral flatness of the decoded PCM. It does NOT work: the AMBE vocoder
  synthesizes speech-SHAPED output (formants, pitch) even from scrambled or
  corrupt parameters, so encrypted/garbled audio and clear speech both read
  ~0.15 flatness. Measured on real captures: clear 0.152/0.172, encrypted
  0.141/0.193 -- fully overlapping. An encrypted call read "good". Dead end;
  the signal is destroyed by the vocoder before the PCM exists.

- **Abandoned (2nd attempt) — b0 pitch-class frame types.** Classifying each
  AMBE frame by its `b0` pitch index (speech/silence/erasure/tone) ALSO fails:
  the b0 of an encrypted frame is pseudo-random (content XOR keystream), so it
  lands anywhere. A real encrypted call on 440.425 showed 18% "silence" by b0
  and read "good". The b0 class is not robust to encryption.

- **Built — AMBE frame REPETITION (`src/audio_quality.hpp`).** Works from the
  vocoder's per-frame AMBE codewords, which dsd-fme emits with `-Z` as
  ` AMBE <hex> err = [a] [b]` lines (already parsed into voice events; `-Z` on
  by default). The robust signal is repetition, not frame class:
  - Clear digital voice repeats the EXACT standard comfort-noise codeword
    (`F801A99F8CE080`) during natural pauses, and repeats sustained phonemes
    frame-to-frame.
  - A cipher XORs each frame, so the exact comfort-noise codeword NEVER appears
    in encrypted audio, and scrambled speech never repeats frame-to-frame.
  The verdict keys on the COUNT of the exact comfort-noise codeword, which a
  cipher can never produce (it scrambles the standard value away): its presence
  means the call had real pauses and is clear. Calibrated on real captures --
  clear calls always carry several comfort-noise frames (a pause is multiple
  20 ms frames), encrypted calls carry exactly zero:
  - `>= 2` comfort-noise frames -> **good** (clear, has natural pauses).
  - `0` comfort-noise frames and `>= 80` frames (~1.6 s) -> **unusable** (a
    multi-second transmission with no pause at all is scrambled; every
    unsignalled-encrypted call flags here).
  - `0` comfort-noise frames and a short burst (< 80 frames) -> **unknown**: a
    quick clear word with no pause and a short encrypted burst look identical,
    so don't risk a false "unusable".
  - `< 50` frames -> **unknown** (too short to judge).
  There is no longer a "marginal" verdict -- it was a false-positive magnet
  for long continuous talkers (one pause in a 30 s over reads ~1% comfort-noise
  and used to land marginal; now it reads good). The silence fraction (`qs`),
  repeat fraction (`qr`) and `err`/frame (`qe`) are kept as diagnostics only;
  `qr` still separates encrypted speech (~0% rep) from encrypted silence
  (repeats) in the hover.
  - Wired into `AssocModel`: fed from the **event stream** in `ingest()` (not
    the PCM), so it runs whenever voice is decoded, independent of recording
    (`DSD_NET_QUALITY=0` disables). Per-call analyzers in `call_quality_`,
    finalized onto `Call::qual` in `close_call`; open calls get a live running
    verdict in the snapshot.
  - `/net.json` per call: `q` (verdict), `qs` (silence frac), `qr` (repeat
    frac), `qe` (err/frame), `qn` (frames); round-trips through export/import;
    merges in `fold_call`. Explorer shows LOW QUALITY / MARGINAL (good shows
    nothing); tooltip gives the numbers and states cause is not determined.
  - A voice call that decoded NO AMBE frames (flagged voice from a sync, but
    no voice bursts -- e.g. a sync-only channel, or too weak to decode any
    audio) gets `q=unknown` and a muted "QUALITY ?" badge, so it reads
    "couldn't assess" rather than showing nothing. A voice call with some
    frames but fewer than the 50-frame minimum stays unbadged (it did get
    bursts, just too few to judge).
  - A call the protocol flags encrypted (SIGNALLED: a key id / algorithm, so
    `enc` is set) that we have no key for gets NO quality verdict -- the
    ENCRYPTED badge already says so, and there is no intelligible audio to
    judge, so a quality tag would be redundant noise. (With a key it decodes
    clear and reads "good".) This suppression keys on the signalled `enc`
    flag only; UNSIGNALLED encryption (no key id, e.g. TYT EP) is never
    flagged `enc`, so it still gets a verdict -- catching it is the whole
    point of this feature.
  - Tests: `tests/test_audio_quality.cpp`, `tests/test_dsd_fme_parse.cpp`,
    `tests/test_assoc_model.cpp` integration -- all keyed to the real stats.
  - Scope: AMBE+2 only (DMR / NXDN / P25 Phase 2). P25 Phase 1 uses IMBE
    (different marker), so those calls get no frames -> verdict "unknown".

- **Calibration (on several real captures, clear + encrypted traffic).**
  | call | comfort-noise codeword frames | verdict |
  |---|---|---|
  | clear speech (~100 calls) | 4-19 (several; always >= 2) | good |
  | unsignalled-encrypted (440.425) | 0 | unusable |
  | short clear word, no pause (57 frames) | 0 | unknown (not flagged) |
  Validated across five real recordings: every unsignalled-encrypted 440.425
  call reads unusable, every clear call reads good, and the two false-positive
  modes are fixed -- long continuous talkers (12-30 s, one pause, ~1% of
  frames comfort-noise) now read good, and a 1 s no-pause clear burst reads
  unknown instead of a false unusable.
  Tunable to widen against: `kNoPauseUnusable` (80 frames ~1.6 s) is the
  boundary for the zero-comfort-noise case. The shortest observed false-
  positive clear-no-pause call was 57 frames and the shortest real encrypted
  call ~90; 80 sits between. A longer clear burst with truly no pause (>= 80
  frames, ~1.6 s) would still false-flag unusable, and a short (< 80 frame)
  encrypted burst reads unknown rather than unusable -- both are the price of
  the zero-comfort-noise ambiguity. A radio using a non-standard comfort-noise
  frame would read 0 -> false flag; the codeword is the AMBE+2 standard, so
  unlikely but unverified across radios.

- **To do — Layer 1 (RF error rate) as a verdict input.** The `err` counts
  are parsed and exposed but don't yet gate the verdict (no weak-signal
  captures to calibrate the RF threshold -- the AWGN gradient below). Adding
  it gives the noisy-link vs. clean-link-content distinction directly.

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
may log richer per-call diagnostics for field debugging, built from these
three signals:

- **FEC / link error rate** — identifies degraded SNR (high correction counts,
  intermittent sync). Direct measurement.
- **Acoustic quality score** — the Layer 2 verdict (speech-like or not).
- **Signalled-crypto metadata (key id / algorithm id)** — when present. For
  signalled traffic (Motorola EP PI header, P25 ESS, DMR privacy indicator)
  the protocol *announces* encryption and dsd-fme already parses the
  key id/algo (it is what the Keys tab is built on). This is reading the
  protocol, not inference.

  **Caveat — signalling is NOT universal.** Simplex / direct (DMO) calls
  routinely carry no privacy signalling, and unsignalled schemes (e.g. TYT EP)
  never do. So the key-id signal is present only for repeater/trunked traffic
  on schemes that signal; it is absent exactly in the case below.

### The diagnostic that matters: noisy link vs. clean-link content problem

The field case that motivated this: a user thumbs privacy ON by accident on a
simplex call. No signalling to read, not a known-EP talkgroup — a plain
misconfiguration. "Radio X is garbage: is it RF, or did someone enable
privacy?" The FEC + acoustic pair answers it directly:

- **high FEC errors + bad audio** -> RF problem (degraded SNR).
- **low FEC errors (clean link) + unintelligible audio** -> NOT RF. The bits
  arrived intact; the problem is at the content level. On one's own network
  with a strong local simplex signal, that is the fingerprint of accidental
  privacy.

This "clean link + unintelligible" state is just the logical AND of the two
measurements above, so recording it as a backend diagnostic is fine and gives
the operator the signal they need to catch a misconfigured radio (simplex
included, where signalling can't help).

### The line that stays: no content recovery, no certainty oracle

What is still NOT built:

1. **No decryption / forced key / content recovery.** The diagnostic recovers
   zero content and defeats no encryption — it only says "unusable, and here
   is whether the link was clean." Keeping it a diagnostic (not an
   interception tool) is the load-bearing guardrail.
2. **Report the observed state, not a certainty verdict.** It records
   "clean link + unintelligible audio — likely a privacy/content problem," NOT
   "ENCRYPTED: yes." This is honest (clean-link-garbage can also be a bad
   decode / wrong codec params / vocoder edge case, so certainty would be a
   lie) and it avoids shipping a binary encryption-presence oracle. The
   operator supplies the final interpretation; on their own network the hedge
   is enough to act on.

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

## Processing cost (estimate)

Yardstick — baseline already running per session: the FM front-end applies a
63-tap complex FIR to the raw IQ at the SDR rate (~2 Msps typical) ->
~126M complex MACs/sec, continuously; plus dsd-fme's Viterbi/FEC + AMBE
vocoder on top. That is the real per-stream cost.

- **Layer 1 (FEC error rate): effectively free.** dsd-fme already computes the
  counts and prints the lines; `classify_dsd_fme_line` already parses every
  line. Addition = pull a number from lines currently dropped as `unknown` +
  a running per-call sum. No per-sample work.

  **dsd-fme verbosity cost.** Layer 1 needs dsd-fme run at a verbosity that
  emits the FEC/sync-error fields (the current command line does not; we drop
  them as `unknown`). This does NOT add decode cost: the Golay/Hamming/BPTC
  correction and sync-error counting *are* the FEC decode and already run —
  verbosity only controls whether the already-computed numbers get printed.
  Marginal cost at a modest level = extra stderr text (tens of lines/sec per
  active call, a few KB/sec) + those lines going through the reader's existing
  `strip_ansi` + `classify_line`. Negligible.
  CAVEAT: do NOT enable full payload/symbol debug (per-dibit / per-codeword /
  hex dumps). That emits orders of magnitude more (hundreds of lines/burst,
  potentially MB/sec), loads the reader thread, and — if the stderr pipe fills
  faster than we drain it — back-pressures dsd-fme (blocks on write), which can
  throttle decode timing. Request the LOWEST verbosity that surfaces the
  FEC/sync-error fields. The verbose capture needed to pin the log-line strings
  also confirms the right level (and that the heavy dumps stay off).
- **Layer 2 (acoustic): small, active-only.** Works on decoded voice PCM
  (8 kHz mono per slot; DMR up to 2 slots). Dominant cost is a 256-pt FFT at
  50% overlap for spectral flatness: ~62 frames/sec/slot -> ~0.5M flops/sec/
  slot -> ~1M flops/sec/session during voice. Flatness / ZCR / energy-mod on
  top are negligible. That is ~0.2% of the FM demod's own ~500M+ flops/sec for
  the same stream, and 0% when idle (no call -> no PCM -> no work). 20
  concurrent active slots still < 1% of one core. Could drop the FFT for an
  8-band IIR/Goertzel filterbank to go cheaper, but no need.
- **Memory: trivial.** All features are streaming/online — no whole-call
  buffering. A few hundred bytes of accumulator per active session.
- **Constraint, not cost:** Layer 2 runs in the audio-callback path, so keep
  it allocation-free there (one reused FFT scratch buffer per session). A
  256-pt FFT is microseconds against a 32 ms frame, so no risk to the audio
  thread.

Bottom line: Layer 1 free, Layer 2 a fraction of a percent of the DSP already
running per stream, both zero when no one is talking.

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
