# TETRA voice decode — design + status

Status: **tetra-kit voice path works via an opt-in codec build; osmo voice
path is the remaining piece.** `src/tetra_voice.*` extracts encoded speech from
tetra-kit's JSON (base64 + zlib) into 690-int16 frames, and `TetraKitProcess`
feeds them to an optional `TetraVoiceDecoder` → `AudioCallback`. The codec
binding now exists: `src/tetra_voice_codec.cpp` (compiled only with
`DSD_WITH_TETRA_CODEC`) adapts tetra-kit's ACELP `audio_decoder` to the seam,
so a codec build emits real 8 kHz audio on `protocol":"tetrakit"` sessions —
validated by `tests/test_tetra_codec.cpp` (real captured frame → decoder →
non-silent PCM). The codec is still **not vendored** (patent-encumbered /
GPLv3 — see CODEC below); it is fetched only in an opt-in build
(`cmake -DDSD_WITH_TETRA_CODEC=ON -DTETRA_KIT_DIR=<tetra-kit>` or the
Dockerfile's `--build-arg TETRA_CODEC=1`, which reuses the tetra-kit it already
clones). A default build still returns null from `make_tetra_voice_decoder()`
and emits events only. The osmo (`protocol":"tetra"`) voice UDP path is not
wired yet, so that backend stays events-only regardless.

**Validated end to end on the real capture** (outside the repo, using
tetra-kit's own GPLv3 codec as an oracle): our demod → tetra-kit decoder →
these extracted frames → the codec produced ~2.7 s of intelligible 8 kHz
speech from the off-air `TCH_S` traffic. So the data path and the extraction
target are confirmed; only the codec binding remains.

The rest of this note is the design and the remaining work.

## Goal

Emit decoded TETRA voice as the same tagged binary audio frames every other
protocol uses: **`0x01` + little-endian 16-bit PCM, 8 kHz mono** (see
PROTOCOL.md "Binary frames — decoded voice audio"). No new wire protocol — the
client already handles `0x01` audio from the DMR/DSDcc paths. The session's
`AudioCallback` seam is already wired through both TETRA backends (it is passed
to `start()` today, just never invoked), so the only new work is *producing*
the PCM.

## The codec problem (read first)

TETRA voice is **ACELP** (ETSI EN 300 395-2, "TETRA speech codec"): 30 ms
frames, ~4.567 kbit/s, 137 bits/frame of class-0/1/2 bits, decoding to 240
samples at 8 kHz. It is **patent-encumbered**; the ETSI reference C source is
freely downloadable but **must not be vendored into this repo**. So the codec
is an **optional external dependency**, found at build time exactly like
DSDcc/mbelib is for the in-process DMR backend:

- CMake option `DSD_WITH_TETRA_CODEC` (default OFF) + `-DTETRA_KIT_DIR=<tetra-kit
  checkout>`, which compiles tetra-kit's `recorder/audio` codec sources (the
  ETSI reference wrapped GPLv3) plus `src/tetra_voice_codec.cpp` into the
  servers. When OFF, the TETRA variants build **events-only**, i.e. the default
  behavior — no regression, no new hard dep. A build with it ON is GPLv3 by
  virtue of the linked codec, which is why it is opt-in and off by default.
- Encrypted traffic (TEA1–4) can't be decoded without keys and is out of
  scope regardless of the codec.

## Where the encoded voice comes from (differs per backend)

This is the crux, and the part with real unknowns.

### tetra-kit (`protocol":"tetrakit"`)

tetra-kit already carries speech **inside the JSON reports** we parse today:
`common/report.cc` emits `"uzsize"` (uncompressed size) and `"zsize"` (zlib
size) alongside a **base64 payload of the zlib-compressed encoded frames**
(U-PLANE reports). So the tetra-kit voice path is entirely in-process:

```
JSON report (service:"U-PLANE") → base64-decode payload → zlib inflate
    → encoded ACELP frame(s) → TetraVoiceDecoder → 8 kHz PCM → AudioCallback
```

- **Pin against real output:** the exact JSON field name for the payload
  (likely `"data"`/`"frame"`), whether it is one 30 ms frame per report or
  several, and the bit packing the codec expects. `tetra_kit_json`'s parser
  today deliberately *skips* array/nested values — the speech payload is where
  that needs extending (extract the base64 string field).
- zlib is already a common dep; base64 is trivial in-tree. No extra process.

### osmo (`protocol":"tetra"`, sq5bpf fork)

The sq5bpf fork sends **voice traffic as a separate UDP stream** (distinct from
the TETMON event datagrams), one stream per traffic channel, which telive
normally routes to the ETSI codec. So the osmo voice path adds a second UDP
receiver to `TetraProcess`:

```
tetra-rx voice UDP  → (per-usage-marker/idx) encoded frames
    → TetraVoiceDecoder → 8 kHz PCM → AudioCallback
```

- **Pin against real output:** the fork's voice UDP port/lifecycle (is it a
  fixed port, a per-call port, env-configured like `TETRA_HACK_PORT`?), the
  datagram framing, and how a voice stream associates with its call (the `idx`
  / usage-marker we already surface in `extra`). telive's receiver scripts and
  the fork's source are the authority.

## Shared piece: `TetraVoiceDecoder`

One thin wrapper, backend-agnostic, so neither backend embeds codec details:

```cpp
// tetra_voice.hpp  (compiled only with DSD_WITH_TETRA_CODEC)
class TetraVoiceDecoder {
public:
    // Feed one encoded TETRA speech frame (30 ms). Appends decoded 8 kHz
    // mono int16 samples (240 per frame) to `pcm`. Returns false on a bad
    // frame. Stateful (the ACELP synthesis filter carries across frames).
    bool decode_frame(const uint8_t* frame, size_t n, std::vector<int16_t>& pcm);
    void reset();
};
```

Both backends do the same last mile: `decode_frame(...)` → hand the PCM to the
existing `AudioCallback`, which already tags it `0x01` and posts it to the
client's strand. When `DSD_WITH_TETRA_CODEC` is off, the class isn't compiled
and the extraction paths above short-circuit (events-only build).

## Sequencing (smallest testable steps first)

1. **`TetraVoiceDecoder` interface + CMake option** — DONE. The seam is in
   `src/tetra_voice.hpp`; `make_tetra_voice_decoder()` returns null without a
   codec build. Zero behavior change when the codec is off.
2. **tetra-kit extraction** (base64 + zlib inflate of the UPLANE payload) →
   690-int16 frames — DONE (`tetrakit_extract_speech_frame`). Unit-tested
   against a real captured frame (`tests/test_tetra_voice.cpp`), and
   `TetraKitProcess` is wired to call it per traffic report.
3. **Wire codec → AudioCallback** — DONE. `src/tetra_voice_codec.cpp` (a
   `DSD_WITH_TETRA_CODEC` build only) implements `make_tetra_voice_decoder()`
   on tetra-kit's `audio_decoder` (690 int16 → 480 int16 PCM), and
   `TetraKitProcess` already routes extracted frames → decoder → `on_audio`.
   `tests/test_tetra_codec.cpp` runs the real captured frame through it and
   asserts non-silent PCM (gated on the codec build).
4. **osmo voice UDP receiver** in `TetraProcess` (second socket, framing pinned
   against the fork), then the same codec → AudioCallback wiring.
5. **Codec round-trip test** when `DSD_WITH_TETRA_CODEC` is available (encode a
   tone → decode → check), else skipped — mirroring how the DSDcc tests gate on
   the optional lib.

## Unknowns to resolve with a real capture / the upstream source

- tetra-kit: the speech JSON field name + framing (one frame per report?).
- osmo: the sq5bpf voice UDP port/lifecycle, datagram framing, call association.
- The ETSI codec's exact entry points and expected bit layout (the reference
  has a specific frame/parameter ordering).
- Half-rate vs full-rate and frame-stealing (control bits stealing a speech
  frame) — handle as dropped/again-flagged frames, don't crash.

## Non-goals

- No vendoring of the ETSI codec (licensing).
- No decryption of TEA-encrypted voice.
- No new client-facing wire format — voice reuses the `0x01` PCM frame.
