# Key-trial design notes (encryption key auto-discovery)

Status: **design pinned, not yet built.** Branch `claude/key-trial` (off `main`).
This file is the hand-off so a future session can resume cold. Written
2026-10-08.

## The problem

We monitor our own (authorized) fleet. We hold the fleet's encryption keys,
but keys get **rotated and swapped around key IDs** over time, so we don't
reliably know which key value is behind a given key ID right now. Goal:

1. Keep a **pool** of our known keys.
2. For an encrypted call, **try each pool key** and find the one that actually
   decrypts the voice -> learn `key id -> key`.
3. Store that mapping (extends the step-2 keyring) so live decode uses it.
4. **Detect when a mapping goes stale** (the system rekeyed that key id) and go
   back to the pool to re-trial — continuously, without flapping.

## Verified facts (don't re-derive these — they're checked against dsd-fme source)

- **There is no cryptographic way to validate a P25/DMR voice key.** The voice
  stream ciphers (P25 AES/DES-OFB, ADP-RC4; DMR BP/EP/AES/RC4) have **no MAC or
  CRC** over the plaintext. So "try each key" == decrypt N ways + judge the
  audio. You cannot shortcut it with a checksum.
- **The vocoder/FEC error counts are computed BEFORE decryption**, so they
  reflect RF channel quality only — they are the SAME for a right or wrong key.
  Verified in dsd-fme `src/dsd_mbe.c` `processMbeFrame()`: the order is always
    `mbe_ecc...C0()` -> `mbe_demodulate...Data()` -> `mbe_ecc...Data()` (-> `imbe_d`/`ambe_d`, sets `state->errs`/`errs2`)
    ...THEN the keystream XOR is applied to the decoded voice bits `imbe_d`/`ambe_d`.
  P25 path ~line 258-261 + the `payload_algid` decrypt block after; DMR/NXDN
  path ~line 471-474 + the cipher block after. So `errs`/`errs2` = RF only.
- Therefore the "is this the right key?" signal must come from AFTER decryption:
  - **Primary, no fork:** speech-vs-noise on the decoded PCM. A wrong key
    desyncs the stream cipher and yields white-noise-like audio for the whole
    call; the right key yields coherent voiced speech. Over 1-2 s this is a
    reliable discriminator (voiced-frame ratio, spectral flatness, energy
    structure). Works on the audio we already produce.
  - **Sharper, needs a fork:** mbelib's invalid/out-of-range vocoder-parameter
    rate (wrong key -> random params -> more invalid frames). dsd-fme computes
    some of this internally (e.g. "IMBE Non-standard c0 detected, skipped" in
    `src/p25p1_ldu.c` ~line 163) but does NOT expose a clean per-call count.
  - **Confirm:** ASR (whisper, already integrated) on the top candidate — right
    key -> real words, wrong key -> gibberish/low confidence.
- **Applying a found key live** with stock dsd-fme requires a decoder restart:
  dsd-fme loads its key list (`-K`) at start only. So each key change -> restart
  the stream's dsd-fme with the updated list -> short decode gap. (This is the
  same "apply" caveat deferred in step 2.) A fork removes this.
- **Single-key forcing exists:** dsd-fme `-H <hex>`/`-1 <hex>`/`-R <dec>`/`-b <dec>`
  with `-4`/`-0`/`-M` applies ONE key regardless of the signaled key id. That is
  what Path A uses to force each candidate key during a trial. (See
  `src/session.cpp start_pipeline` ~line 1430-1575 for how we already pass these.)

## The judge (shared by both paths)

A score per candidate decrypt = speech-likeness of the decoded PCM, gated by RF
quality:
- compute a DSP speech/noise score over the call's PCM;
- **ignore calls with high FEC `errs`** (RF-poor) — they tell you nothing about
  the key, and judging on them causes false staleness;
- pick the candidate with the best score above a threshold; require it to pass on
  a couple of calls before trusting (anti-flap);
- optional: ASR confidence on the winner as a final confirm.

## Per-(network, key id) state machine

- `unknown` — encrypted calls seen, no key yet. Trial the pool when we have a sample.
- `trialing` — running the pool against a captured sample.
- `mapped(key)` — a pool key won; live decode uses it; keep a rolling health score.
- `no-key-in-pool` — pool trialed, nothing decrypted it (distinct from `unknown`;
  this is the OTAR case — the system rekeyed to a value we don't hold).
- `stale` — a mapped key's health dropped (N consecutive clean-signal calls score
  as noise) -> demote, re-capture, re-trial.

Staleness detection reuses the judge on live mapped calls. Needs the live decoder
to emit per-call audio for mapped-encrypted calls (today encrypted calls are
`no_audio`; once mapped, treat as decryptable -> record/score). Hysteresis (N
consecutive, RF-gated) sets reaction latency vs twitchiness; make it configurable.

Pool semantics: the pool is the full fleet key set, **never pruned on demotion**
(rotated-away keys come back). A key moving across key ids is found automatically
(every pool key is tried against any key id). A brand-new key not in the pool ->
`no-key-in-pool` (surface it so we know we're missing a key).

## Data model (extend the step-2 keyring — `src/assoc_keys.hpp`)

Today the keyring is `fam -> net -> keyid -> {alg, value}` (see `KeyRing`,
`keyring_set/_csv`, and `AssocModel::set_key/keys_csv` in `src/assoc_model.hpp`,
endpoints in `src/session.cpp` `/net/keys/*`, UI `keysSection` in
`src/net_page.hpp`). Add:
- a **candidate pool** per network/fleet (keys unbound to a key id);
- per `(net, keyid)`: `state`, rolling `health`, last-promoted/last-stale times,
  consecutive-clean-noise count;
- a **sample reference** to re-trial from (IQ window for Path A, buffered frames
  for Path B).
Keep key values write-only / owner-only / never exported (as step 2 already does).

## Two paths

### Path A — trial by replay (no fork)
Keep a short rolling **IQ** buffer per channel (existing `iq_log`/MIDAS BLUE:
`src/blue_writer.hpp`, `/iq_log/on` in `src/session.cpp` ~line 832, IQ demod
`src/fm_demod.cpp`). On `unknown`/`stale`, take a captured encrypted call and run
its IQ back through FmDemod -> dsd-fme once per candidate key (force the key),
score each PCM, pick the winner, write to the keyring, restart the live decoder to
apply.
- Pros: no fork; reuses proven components; proves the judge + state machine fast;
  all logic in our code.
- Cons: N full demod+decode passes per trial (heavy); IQ buffer is big (~MB/s per
  channel); recovery is background (seconds-tens); applying a key needs a decoder
  restart (gap); scales poorly.

### Path B — trial in a fork (dsd-fme and/or DSDcc)
Fork dsd-fme (P25 AES/DES/RC4) and/or DSDcc (DMR BP). After FEC decode buffer the
tiny encrypted voice bits + MI per call; decrypt once per candidate key inline,
score (mbelib parameter validity + quick audio metric), pick inline, keep decoding
with the winner — no restart.
- Pros: cheap per-call samples; inline live recovery within a call or two; no
  restart/gap; sharper judge; scales well.
- Cons: real fork of C crypto/vocoder internals; two backends; higher risk +
  maintenance; GPL; longer to first result.

Most groundwork (pool, state machine, judge, RF-gating, keyring data model + UI)
is **shared** — only the trial engine differs, so Path A is genuine progress
toward B.

### Middle option — minimal dsd-fme patch (not a full fork)
Patch dsd-fme to (a) dump encrypted frames + MI per call and (b) accept the full
key list and try all keys for an unknown/stale key id, picking by the vocoder
metric. A fraction of a full fork; gets the cheap-sample + inline-apply wins.

## Recommendation

Phase it:
1. **Phase 1 = Path A** — build the shared machinery (pool, judge, staleness/
   re-trial state machine, keyring/UI), prove the judge is reliable and the loop
   doesn't flap, with no fork / low risk.
2. **Phase 2 = Path B** (or the minimal patch) — move the trial engine into a fork
   for cheap continuous re-trial, inline apply, and scale; reuse all of Phase 1.

If the fleet rotates rarely and background recovery is fine, Path A may suffice
alone. If rotation is frequent and live recovery is wanted, Path B is the
destination and A is the de-risking step.

## OPEN DECISION before building (need from the user)

- **How often does the fleet rotate keys?** (ad hoc / daily / weekly / ...)
- **Recovery expectation:** live (within a call) or background (seconds-tens)?

Rare + background-ok -> Path A. Frequent + live -> A to prove, then B / minimal patch.

## Legality framing

For the user's own fleet, which they are authorized to monitor. Keep the keyring
write-only / owner-only / out of exports, as step 2 already does.

## Fast pointers (our repo, this branch's base = main @ 7476a50)

- Keyring data + CSV: `src/assoc_keys.hpp`
- Keyring model methods (`set_key`, `remove_key`, `keys_csv`, `/net.json keyed`):
  `src/assoc_model.hpp`
- Key endpoints (`/net/keys/set|remove|list`): `src/session.cpp`
- Keyring UI (`keysSection`, add/remove/download, `keyedHas`): `src/net_page.hpp`
- Encryption parse (alg_id/key_id for P25/DMR/NXDN): `src/dsd_process.cpp`
  (`classify_dsd_fme_line`, regexes near "P25 encryption identifiers")
- IQ capture: `src/blue_writer.hpp`, `/iq_log/*` in `src/session.cpp`
- Demod (IQ -> discriminator): `src/fm_demod.cpp`
- Decode pipeline / key flags: `src/session.cpp` `start_pipeline`
- Replay tool (events): `tools/net_replay.cpp`
- ASR (whisper) integration: `src/net_page.hpp` (`ASR.*`), `tools/get_asr_assets.sh`

## dsd-fme source pointers (if forking / reading internals)

A clone was at `/tmp/dsd-fme` (git `0838a7f`, 2026-09-21). Re-clone if gone.
- Vocoder + decrypt order: `src/dsd_mbe.c` `processMbeFrame()` (P25 ~258-261,
  DMR/NXDN ~471-474; decrypt blocks follow each).
- P25 IMBE frame handling + non-standard detect: `src/p25p1_ldu.c`, `p25p1_ldu2.c`
  (LFSR/AES IV from MI ~730-803).
- Key import (CSV `-K`/`-k`, indexed by key id): `src/dsd_import.c`
  `csvKeyImportHex`/`csvKeyImportDec`.
- Single-key + force flags parse: `src/dsd_main.c` (cases `1`,`H`,`R`,`b`,`S`,
  `0`,`4`,`M`).
- Crypto: `src/crypt-aes.c`, `crypt-des.c`, `crypt-etc.c`.
