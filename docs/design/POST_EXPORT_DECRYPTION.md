# Design notes: decrypting calls after export

Status: **not started**. These are notes to come back to.

## The ask

A user records a session, forgets to enter decryption keys, and exports it.
Later they remember the keys. They want to load the export, enter the keys,
and hear the encrypted calls.

## Why today's exports can't be decrypted

- Decryption happens inside dsd-fme, on the encoded voice frames (AMBE for
  DMR, IMBE/AMBE for P25), before the vocoder turns them into audio.
- Without a key, dsd-fme mutes encrypted voice. The server also deliberately
  records no audio for an encrypted call unless a key was applied:
  `AssocModel::audio()` in `src/assoc_model.hpp` (the `k->encrypted && !keyed`
  branch sets `no_audio` and discards anything held).
- So an export carries the call records (source, talkgroup, `enc`, algorithm,
  key id) but none of the encrypted voice. There is nothing to decrypt later.

## Option A: re-decode an IQ capture with the keys

- Only possible when IQ capture was on for the session (`iq_log` in the
  `start` message; see "IQ capture" in `PROTOCOL.md` and README). The BLUE file
  holds the raw signal, so the calls can be decoded again.
- Feature: replay a capture through dsd-fme with the current keys (the same
  path as `tools/midas_ws_client.py`, or a server-side job), then attach the
  recovered audio to the matching exported calls by time, source and
  talkgroup.
- Works for every key type we support, since it is just a normal decode with
  keys.
- Effort: about one or two PRs. Most of the work is matching recovered calls
  to exported ones (clock offset, split or merged calls).
- Catch: captures are large, so most sessions won't have one. It doesn't help
  the "forgot the keys" case unless capture was already running.

## Option B (recommended): keep the encrypted voice and decrypt later

### What to capture

- The server already runs dsd-fme with `-Z` (`src/dsd_process.cpp`, added for
  DMR short data). `-Z` makes dsd-fme print every voice frame
  (`PrintAMBEData` in dsd-fme's `src/dsd_file.c`: ` AMBE %014llX err = [..]`).
  For a call with no key, those frames are the still-encrypted ciphertext.
  - **To verify:** where the print happens relative to decryption in each
    path (mono vs stereo/slot path in `processMbeFrame`, `src/dsd_mbe.c`), so
    we are sure we capture ciphertext, not half-processed output.
- Per encrypted call, store:
  - the ordered voice frames, per slot (about 7 bytes per 20 ms frame, ~350
    bytes per second of speech);
  - algorithm id, key id;
  - the IV / MI as it changes. DMR Motorola EP / AES / DES carry an MI that
    late entry (VC6) updates every superframe; P25 carries it in the ESS.
    The forced kinds (BP, TYT EP/BP/AP, Anytone BP, Baofeng / Retevis AP) use
    no IV.
- Include it in the export (a new optional per-call field, e.g. `ct` for
  ciphertext; exports without it still load).

### How to decrypt later

- dsd-fme can already play recordings in SDRTrunk's `.mbe` JSON format with
  keys: `dsd-fme -r file.mbe` plus the key options (`-K`, `-1`, `-H`, `-b`,
  `-5`, ...). Reader: `read_sdrtrunk_json_format` in dsd-fme's
  `src/dsd_file.c`, which handles alg 0x21 (RC4), 0x22 (DES), 0x24 / 0x25
  (AES) with an IV, and the forced-key paths.
  - **To verify:** whether that reader handles an IV that changes partway
    through a call, or only one IV per file. If only one, split a call into
    one file per superframe, or decrypt ourselves.
- "Decrypt with keys" in the file view: for each encrypted call with
  ciphertext, write the `.mbe` JSON, run `dsd-fme -r` with the matching key
  (from the keyring by key id, or the network's no-key-id key: BP, EP, TYT EP,
  etc.), capture the WAV, attach it as the call's audio.
- Where it runs: server side (the server has dsd-fme and the keyring). A file
  view is client-side today, so this needs an endpoint that takes an export
  (or the selected calls' ciphertext), runs the decrypt, and returns audio.
  The result could be saved as an "export with audio" zip.

### Staged plan

1. **Store and export the ciphertext.** Parse `-Z` frames per slot in the
   stdout/stderr reader, keep them on encrypted calls with alg / key id / IV,
   and add them to exports. No UI change.
2. **Decrypt the no-IV key types** (BP, TYT EP, TYT BP, Anytone BP, TYT /
   Baofeng / Retevis AP). Simplest: static keystream, no IV handling.
3. **Decrypt Motorola EP, AES and DES** with changing IVs. Only once there is
   a real encrypted recording plus its key to test against.

### Risks / open questions

- Reliably attributing `-Z` frame lines to the right slot and call in
  dsd-fme's text output (stereo DMR prints both slots interleaved).
- P25 phase 1 (IMBE) and phase 2 (AMBE) frame formats in `-Z` output.
- Storage: ciphertext for every encrypted call adds up on a busy system.
  Respect the existing call cap and maybe make it opt-in.
- Test data: we have no encrypted captures. Need one per stage from the user
  (with its key), or a synthetic generator.
- The SDRTrunk JSON field names and version handling in dsd-fme's reader
  need checking against the pinned commit
  (`198f0eacb5ef3873fab23186640c90789152894c`).
