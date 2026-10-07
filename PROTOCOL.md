# dsd-server WebSocket protocol reference

Every frame the server can send — and, for context, every frame it
accepts — with exact shapes, when each occurs, and real captured
examples. The single source of truth for what goes on the wire is
`session.cpp` (`handle_text_message`, `start_pipeline`, and the event
serializer in it); this document is generated from reading that code and
from running the verified test suite against real dsd-fme and real DSDcc
decoding a real DMR capture, so the examples below are genuine wire
frames, not invented ones.

One WebSocket connection = one session = one demod + one DSD decoder.
Text frames carry JSON; binary frames carry raw sample data. All JSON
objects are **flat** (no nesting, no arrays), and every documented key
is **always present** in the frames that carry it — a field whose value
is unknown is an empty string `""`, never omitted and never `null`. The
one exception is `crc_error`, a boolean flag always reported as `"0"` or
`"1"` (never `""`).

**Machine-readable description.** This narrative is the source of truth, but
the same interface is also described formally under [`api/`](api/):
`api/asyncapi.yaml` (AsyncAPI 2.6 — the format built for bidirectional,
message-driven WebSocket protocols) and `api/index.html`, a self-contained
interactive viewer (no build step). A protobuf mirror of the JSON frames lives
in [`proto/dsd_server.proto`](proto/dsd_server.proto). All are client-side
convenience artifacts; the server itself hand-writes and hand-parses its JSON.

---

## Client → Server

### Text frames (JSON control messages)

Each is a flat object selected by `"type"`. Unknown numeric fields fall
back to the defaults shown (see `handle_text_message` in `session.cpp`).

| type | fields | effect |
|---|---|---|
| `start` | `sample_rate` (default 2000000), `channel_bandwidth` (12500), `freq_offset` (0), `gain` (26000), `afc` (false), `matched_filter` (false), `protocol` (""), `key_type` (""), `key` (""), `pocsag_mode` (""), `invert` (false), `iq_log` (false), `center_freq` (0) | Builds the demod + decoder pipeline. If a pipeline is already running it is stopped and rebuilt (clean restart). Replies with `started` on success, `error` on failure. `iq_log` (default `false`): capture this session's raw IQ to a MIDAS BLUE file (see below). `center_freq` (optional): the absolute tuner centre frequency in Hz, so the channel's frequency is known (`center_freq + freq_offset`); the network explorer labels and keys what it hears by it (see `/net.json`). Nothing else uses it. |
| `set_gain` | `gain` (26000) | Live-adjusts discriminator gain. No reply. Ignored (silently) if no pipeline is running. |
| `set_freq_offset` | `hz` (0) | Live-adjusts the NCO shift. No reply. Ignored if no pipeline is running. Also resets any accumulated AFC correction (an explicit retune is a statement of new truth). With a `center_freq`, the network explorer treats it as a move to another channel. |
| `stop` | — | Tears down the pipeline (kills the dsd-fme child / destroys the decoder). No reply. The WebSocket stays open; a new `start` is accepted afterwards. |

`freq_offset` / `hz` sign convention: **positive means the channel of
interest sits above 0 Hz in your IQ**, and the server mixes it down to
baseband. (An earlier revision had this sign inverted; it is now unified
as stated and pinned by `tests/test_afc.cpp`.)

`protocol`: an **advisory hint** telling the server which digital voice
protocol the client believes the signal is. For the FM/DSD modes it selects
which decode mode to run instead of relying on auto-detection; for `tetra` /
`tetrakit` it does more — it switches the session's **entire signal chain**
(see [TETRA](#tetra-protocoltetra--protocoltetrakit) below). It is forgiving —
case-insensitive, and spaces/underscores/hyphens are ignored:

| value | selects | chain | dsd-fme | DSDcc |
|---|---|---|---|---|
| `""` / absent | server default (**DMR**) — no change for existing clients | FM + DSD | `-fs` | DMR |
| `dmr` | DMR | FM + DSD | `-fs` | DMR |
| `nxdn48` (or `nxdn`, `idas`) | NXDN48 / IDAS (6.25 kHz) | FM + DSD | `-fi` | NXDN48 |
| `nxdn96` | NXDN96 (12.5 kHz) | FM + DSD | `-fn` | NXDN96 |
| `p25` (or `p25p1`) | P25 Phase 1 | FM + DSD | `-f1` | P25p1 (mode only, no fields) |
| `p25p2` | P25 Phase 2 (6000 sps TDMA) | FM + DSD | `-f2` | P25p1 (mode only, no fields) |
| `dpmr` | dPMR (6.25 kHz FDMA) | FM + DSD | `-fm` | dPMR |
| `dstar` (or `d-star`) | D-STAR | FM + DSD | `-fd` | D-STAR |
| `ysf` (or `fusion`, `c4fm`) | Yaesu System Fusion | FM + DSD | `-fy` | YSF |
| `provoice` (or `pv`) | EDACS ProVoice digital voice | FM + DSD | `-fp` | — (auto) |
| `edacs` (or `edacs_std`, `edacs_net`) | EDACS Standard/NET + ProVoice | FM + DSD | `-fh` | — (auto) |
| `edacs_esk` | EDACS Standard/NET + ProVoice, 0xA0 ESK mask | FM + DSD | `-fH` | — (auto) |
| `edacs_ea` | EDACS Extended Addressing + ProVoice | FM + DSD | `-fe` | — (auto) |
| `edacs_ea_esk` | EDACS EA + ProVoice, 0xA0 ESK mask | FM + DSD | `-fE` | — (auto) |
| `x2tdma` (or `x2`) | Motorola X2-TDMA (legacy) | FM + DSD | `-fx` | — (auto) |
| `auto` / `unknown` / `not sure` / anything else | auto-detect | FM + DSD | `-fa` | auto |
| `tetra` (or `osmo_tetra`) | **TETRA** via osmo `tetra-rx` | π/4-DQPSK + TETRA | — | — |
| `tetrakit` | **TETRA** via tetra-kit `decoder` | π/4-DQPSK + TETRA | — | — |
| `pager-auto` (or `pager_auto`, `pager`, `paging`) | **Paging**: POCSAG 512/1200/2400 + FLEX at once | FM + multimon-ng | — | — |
| `pocsag` | POCSAG, all three rates | FM + multimon-ng | — | — |
| `pocsag512` / `pocsag1200` / `pocsag2400` | POCSAG at one rate | FM + multimon-ng | — | — |
| `flex` | FLEX (1600/3200/6400 bps, 2/4-level) | FM + multimon-ng | — | — |

`tetra` / `tetrakit` are not FM modes: they replace the FM discriminator with
the π/4-DQPSK modem and the DSD backend with a TETRA subprocess decoder, so
the dsd-fme/DSDcc columns don't apply (`—`). Their IQ-rate and `event`-field
differences are detailed in the [TETRA section](#tetra-protocoltetra--protocoltetrakit).

The paging hints likewise run their own chain (an FM discriminator tuned for
multimon-ng + a multimon-ng subprocess) and emit `kind:"page"` events; see the
[Paging section](#paging-protocolpager-auto--pocsag--flex). Note plain `auto`
remains the **DSD** auto-detect — paging is selected only by these explicit
hints.

`provoice` / `edacs*` / `x2tdma` are **dsd-fme-backend only** — DSDcc has no
decoder for them, so on the `dsd-server-dsdcc` build they fall back to
auto-detect (`— (auto)`) rather than mis-decoding as DMR; use the default
subprocess build for these. EDACS identifiers ride in the usual fields
(`Group`→`talkgroup`, `Source`/`Caller`→`source_id`, `Callee`/`Target`→
`talkgroup`) with `lcn`/`afs`/`lid`/`system_id` in `extra`; that mapping is
pinned to dsd-fme's own print formats and unit-tested, but not yet validated
against a live EDACS/ProVoice signal.

For the FM/DSD hints the choice only steers mode selection; it does not change
the wire format or the `event` shape. Note the two backends differ in NXDN
capability:
the subprocess **dsd-fme backend decodes NXDN reliably** (it applies the
matching input matched-filter per mode and was verified against a real
off-air NXDN48 capture — recovering source, talkgroup, RAN, site/system
codes and adjacent-site info). The in-process **DSDcc backend's NXDN
support is fragile on real signals** — it locks on clean/synthetic input
but drops sync on real captures — so prefer the dsd-fme backend for
anything but DMR.

`key_type` / `key`: an **optional decryption key**. `key_type` names the
scheme and `key` is its value; both absent/`""` (the default) means no key
and unchanged behavior. `key_type` parsing is forgiving (case-insensitive,
spaces/underscores/hyphens ignored). A named `key_type` with a
missing/invalid `key` is rejected with an `error` reply and the pipeline
is not started. A valid `key` is decimal or hex digits (AES/Hytera keys
may contain the spaces that separate dsd-fme's 64-bit hex words); no other
characters are accepted.

| `key_type` | scheme | value format | dsd-fme | DSDcc |
|---|---|---|---|---|
| `bp` (or `basic_privacy`) | DMR Basic Privacy | key **number**, decimal `1`–`255` | `-b` | ✅ `setDMRBasicPrivacyKey` |
| `rc4` | RC4 (DMR/P25/NXDN) | hex | `-1` | — |
| `des` | DES | hex | `-1` | — |
| `aes` (or `aes128`/`aes256`) | AES-128 / AES-256 | hex (space-separated 64-bit words) | `-H` | — |
| `hytera` | Hytera Basic Privacy | hex | `-H` | — |
| `scrambler` (or `nxdn_scrambler`, `dpmr_scrambler`) | NXDN/dPMR EHR scrambler | decimal | `-R` | — |

**Capability gap — read before relying on this.** Only **DMR Basic
Privacy** decrypts on **both** backends; every other scheme is
**dsd-fme-backend only** (the in-process DSDcc library has no
RC4/AES/DES/scrambler support — its sole decryption is the BP key table).
On the DSDcc backend a non-`bp` `key_type` is ignored (logged to stderr)
and the stream decodes without a key. DMR Basic Privacy is not an
arbitrary key: the number selects one of DSDcc's / dsd-fme's built-in
well-known BP keys. Because BP carries no reliable in-band "encrypted"
flag, a BP key is XOR-applied to **every** DMR voice frame — so setting
one on an *unencrypted* channel garbles the audio; only set it when the
channel actually uses BP. The BP path is verified end-to-end on the DSDcc
backend (`tests/test_bp_key_dsdcc.cpp`); the dsd-fme key flags are
verified against dsd-fme's own `-h`/source and confirmed accepted by the
real binary, but — lacking an encrypted capture with a known key — actual
decryption of a live encrypted signal is not asserted here. The key value
is passed to dsd-fme as a separate argv token (never a shell string).

`afc`: when `true`, the demod continuously measures the residual carrier
offset in its own discriminator output and steers the NCO to remove it —
correcting SDR reference (ppm) error and slow drift on top of whatever
`freq_offset` the client supplied. Measured behavior (see
`tests/test_afc.cpp` and the README): locks a static error of up to
~4 kHz in 0.3–1.1 s, tracks drift up to ~250 Hz/s with under ~250 Hz of
residual, holds still on no-signal noise (variance-gated), and is
clamped to ±5 kHz of correction. With AFC on, a signal mis-tuned by
3 kHz — which decodes only partially or not at all otherwise — decodes
in full.

`matched_filter` (**experimental, default `false`**): when `true`, a
root-raised-cosine filter matched to the digital-voice symbol pulse
(4800 Bd, 0.2 rolloff — DMR 4FSK) is applied to the discriminator output
before the decoder, narrowing the post-detection noise bandwidth to the
symbol band. It's a no-op in normal conditions and buys a few dB of
weak-signal margin near the decode threshold; leave it off unless you're
chasing marginal signals. Applies only to the FM/DSD chain (ignored by
TETRA). See the README's matched-filter section for the measured
A/B results and caveats.

Anything else — an unknown `type`, or a text frame that doesn't parse as
a flat JSON object — gets an `error` reply (see below); the connection
stays open either way.

### Binary frames (raw IQ)

Interleaved little-endian `float32` I/Q pairs (`I0,Q0,I1,Q1,...`) at the
`sample_rate` given in `start`. The byte length must be a multiple of 8
(two floats per complex sample); a violating frame gets an `error`
reply and is dropped. Binary frames sent **before** any `start` are
silently ignored — no error, no reply (documented behavior of
`handle_binary_message`).

### TETRA (`protocol":"tetra"` / `protocol":"tetrakit"`)

TETRA is π/4-DQPSK, not an FM mode, so it needs its own front end. It is
**selected at run time by the `protocol` hint**, not a separate binary: the
same `dsd-server` that decodes DMR/NXDN/… also decodes TETRA when a session
starts with `protocol":"tetra"` or `protocol":"tetrakit"`, switching that
session's whole signal chain to the π/4 modem + a TETRA subprocess backend.
(The FM/DSD *backend* choice — dsd-fme vs DSDcc — is still build
time; the TETRA *decoder* choice is this runtime hint.) The two TETRA hints
share the identical π/4 front end and wire protocol, differing only in the
external decoder they drive:

- **`protocol":"tetra"`** — osmo-tetra's `tetra-rx` (sq5bpf fork): bits on
  stdin, events over its TETMON UDP protocol. Its focus is the control plane:
  network/cell broadcasts (`NETINFO1`/`FREQINFO1`) come through as `kind:"sync"`
  with the colour code in `color_code` and `mcc`/`mnc`/`la`/`dlf` in `extra`;
  call-control PDUs (`DSETUPDEC`/`DCONNECTDEC`/…) are `kind:"call"` with the
  calling `SSI` → `source_id` and called `SSI2` → `talkgroup`.
- **`protocol":"tetrakit"`** — tetra-kit's `decoder`: bits and JSON reports
  both over UDP (`-r`/`-t`). Its focus is full PDU decode: traffic
  (`UPLANE`/`TCH_S`) comes through as `kind:"voice"`, `source_id` from `ssi`,
  with `service`/`pdu`/`usage_marker` in `extra`.

The two backends surface *different* views of the same signal — osmo the
network/cell broadcasts, tetra-kit the traffic channel — so pick per what you
need per session. The chosen decoder (`tetra-rx` / `decoder`) must be on the
server's `PATH`; if it can't be spawned the session replies with an `error`
frame and stays open.

The wire protocol is otherwise the same as the FM/DSD modes — `start` /
`stop`, binary IQ in, `event` frames out — with these differences:

- **IQ rate:** the demod is π/4-DQPSK at 18000 sym/s, so `sample_rate` must be
  `samples_per_symbol × 18000` (e.g. 72000 for 4 sps). The server derives
  `samples_per_symbol` from `sample_rate`. Tune near zero IF; the demod pulls
  in residual offset up to ±2250 Hz itself.
- **Ignored `start` fields:** `channel_bandwidth`, `freq_offset`, `gain`,
  `afc`, `matched_filter`, `key_type`, `key` don't apply to the TETRA chain
  and are accepted-and-ignored (TETRA TEA encryption is not handled).
  `set_gain` / `set_freq_offset` are likewise accepted no-ops. (`protocol`
  itself is what selected this chain.)
- **`started`:** `udp_audio_port` is always `0` (the backend binds its own
  internal control socket).
- **`event` fields:** the osmo backend reports `source_id` = the party SSI,
  `talkgroup` = the group SSI (GSSI) when present, and `extra` carries
  `idx=`/`cid=`/`nid=` tokens. `kind` is `call` for call-control messages;
  PHY/AFC diagnostics classify as `unknown` and are suppressed by default.
- **Audio:** not yet emitted — TETRA voice is a separate ACELP path needing
  the (patent-encumbered) ETSI codec. See [`TETRA_VOICE.md`](TETRA_VOICE.md)
  for the design.

**Status:** the streaming modem is **validated on a real off-air capture** —
a 3 s, 12 dB-SNR UK TETRA downlink (≈40.7 kHz IQ, resampled to 72 kHz).
Our π/4 demod locks the burst grid (18 confirmed bursts, ~140 Hz CFO), and
the resulting bits, fed to a real tetra-kit `decoder`, decode coherently:
`MAC-SYNC` (ColorCode 23, MCC/MNC 234/78, LA 6163), `D-NWRK-BROADCAST`
neighbour-cell lists, and `UPLANE`/`TCH_S` traffic — the Viterbi/CRC/descramble
all passing proves the bits are TETRA-correct, not merely grid-locked. The
tetra-kit JSON parser's `service`/`pdu` → `kind` mapping is pinned against that
output. The **full tetra-kit session** (`protocol":"tetrakit"`) was also driven
end to end — that capture streamed over a WebSocket comes back as `event`
frames (`kind:"voice"` for the `TCH_S` traffic; broadcasts suppressed) — which
is what surfaced the decoder's 1024-byte UDP read limit (now respected via
`bits_datagram_bytes`). The **osmo session** (`protocol":"tetra"`) was likewise
driven end to end on the same capture: it returns 158 `kind:"sync"` events
(`NETINFO1`/`FREQINFO1`, ColorCode 17, MCC/MNC 234/78, DLF 393.5125 MHz), and
its TETMON `func`→`kind` mapping is pinned against that real output. Still
open: the **call-control** mapping (`DSETUPDEC` etc. on both backends) needs a
capture with a live call, and **voice PCM** is not emitted yet
(see [`TETRA_VOICE.md`](TETRA_VOICE.md)). The demod is streaming (timing,
differential, CFO and AGC state carry across IQ frames), so decoding is
continuous across frame boundaries.

The **coherent (Costas) path** is the default for TETRA sessions
(`DSD_TETRA_COHERENT=0` forces plain differential), and is validated on
that same off-air capture: it auto-resolved the π/4 parity from burst-grid lock
(picking the correct one — proven by a coherent decode of the real network),
locked the identical 18-burst grid, and through the real `tetra-rx` decoded the
same identity (MCC/MNC 234/78, ColorCode 0x17, DL 393.5125 MHz) while recovering
**more** CRC-protected control-plane messages than differential — SYNC 84→88,
SYSINFO 103→109, D-NWRK-BROADCAST 21→22 (208→219 total, +5%), with no extra
decode errors. That is the predicted low-BER gain showing up on real RF at
12 dB SNR (traffic-channel decode via tetra-kit was identical, TCH_S being
robust at that SNR). See `src/tetra_frontend.*`.

Broadened across **six** off-air windows from the same recording (~30–60 s in):
every one locks cleanly (17–18 bursts, coherent parity 0) and decodes the same
network (MCC/MNC 234/78, ColorCode 0x17, DL 393.5125 MHz) — good demod
robustness on real RF. The coherent gain is **SNR-dependent**, exactly as the
theory predicts: ~5% more messages on the more marginal window, but only
+0.8% (within per-capture noise) across the five cleaner control-channel
windows, where both detectors already saturate near-zero BER and there is
little headroom left. All six are **MCCH control-channel** captures with air
encryption signalled and no in-the-clear traffic channel, so the **CMCE
call-control** mapping (`DSETUPDEC`/`D-SETUP` etc.) and **voice PCM** still
await a capture of an unencrypted traffic carrier carrying a live call.

A second, independent capture set later confirmed all of this on the same
network: **29 IQ snapshots** (complex float32 at a non-round **40690.104 Hz**,
resampled to the demod's rate) across **three carriers** — two MCCH control
channels and, this time, an active **traffic** channel (392.5625 MHz, decoding
`ACCESS-ASSIGN DL_USAGE:Traffic`, i.e. a live call in progress). Our π/4 demod +
the real osmo `tetra-rx` decoded them cleanly — SYNC (MCC/MNC 234/78, CC 0x17),
BNCH SYSINFO (DL 393.5125 MHz, LA 6163), and D-NWRK-BROADCAST neighbour-cell
lists, all CRC-valid — re-confirming the demod on a fresh rate/format. But every
SYSINFO again carried **Air encryption: 1**, so the call's CMCE setup PDUs stay
encrypted and the call-control mapping remained undecodable even with a live
traffic carrier present. The gap is therefore **not a capture-availability
problem but an encrypted-network one**: it needs a TETRA network that runs its
control plane in the clear (`Air encryption: 0`).

### Paging (`protocol":"pager-auto"` / `pocsag*` / `flex`)

Paging is a data service, not voice: the session decodes pager messages and
emits them as ordinary `event` frames with **`kind:"page"`** — same flat shape,
same thirteen fields — so a client handles pages alongside every voice
protocol. The chain is an FM discriminator (sibling of the DSD one, but
producing the 22050 Hz audio multimon-ng expects) feeding a per-session
[multimon-ng](https://github.com/EliasOenal/multimon-ng) subprocess run with
`--json`. `multimon-ng` must be on the server's `PATH` (or named by
`$MULTIMON_NG`) and must be newer than the 1.3.0 distro package (it needs
`--json` and the `FLEX_NEXT` decoder; the Docker image builds it from source).

- **IQ:** same binary IQ as every other mode (interleaved LE `float32` at
  `sample_rate`). Any rate from ~16 kHz up works; the default 2 MS/s is fine.
- **`start` fields that apply:** `sample_rate`, `channel_bandwidth` (12500
  suits 12.5 kHz paging channels; use ~25000 for wide ones), `freq_offset`,
  `afc`, plus two paging-only fields:
  - `pocsag_mode` (`""`/`auto` default, `alpha`, `numeric`, `skyper`): how
    POCSAG message text is interpreted. In `auto`, multimon-ng guesses from
    content, and an ambiguous message **may produce more than one `page`
    event** (one per plausible rendering, differing in
    `extra` `message_type`). Force a mode if you know your network.
  - `invert` (default `false`): negate the discriminator, for spectrally
    inverted (I/Q-swapped) IQ. FLEX detects polarity itself; POCSAG doesn't.
- **Ignored `start` fields:** `gain` (the paging demod scales PCM by
  deviation in Hz, so the level is right at any IQ rate; the DSD `gain` is a
  dsd-fme-specific factor), `matched_filter`, `key_type`, `key`. `set_gain`
  is an accepted no-op; `set_freq_offset` retunes as usual.
- **`started`:** `udp_audio_port` is `0`. No audio is ever emitted.
- **`stop`:** multimon-ng is sent EOF first, so a page it was still
  assembling is flushed and sent before the pipeline goes away.

Event mapping (`kind:"page"`):

| field | value |
|---|---|
| `talkgroup` | the pager address — POCSAG RIC / FLEX **capcode** (decimal). `""` for a POCSAG message whose address codeword was lost. |
| `message` | the page text (`""` for a tone-only page). POCSAG fill padding (`<NUL>`, `<EOT>`, …) is trimmed; other control characters stay as multimon-ng renders them (`<LF>`, …). |
| `emergency` | `"1"` for a FLEX priority message. |
| `crc_error` | `"1"` when FLEX reports a failed message checksum. |
| `extra` | `protocol` (`pocsag`/`flex`), `baud`, `message_type` (`alpha`, `numeric`, `tone`, `skyper`, `binary`, `secure`, `instruction`, `short_message`), `function` (POCSAG function bits 0–3); FLEX adds `flex_type` (exact FLEX type, e.g. `special_numeric`), `levels` (2/4), `phase`, `cycle`, `frame`, `addr_type`, `group` (`1` for a group message), `fragment`. Plus `payload=encrypted_or_binary` when the text looks like ciphertext, or for a FLEX secure/binary message (see below). |
| `raw` | multimon-ng's JSON line, verbatim (it carries everything, including FLEX `group_capcodes`). |
| others | `""` (`source_id`, `slot`, `color_code`, `ran`, `nac`, `alias`). |

**Encrypted pages.** multimon-ng decodes every payload as text whether or
not it is one, so an encrypted page (common on US hospital/EMS "secure
paging") still arrives as a normal `page` event. The capcode, function and
time are valid, because the address isn't encrypted, but `message` is
ciphertext rendered as characters. Such events carry
**`payload=encrypted_or_binary`** in `extra`, so a client can avoid showing
`message` as text. The detector is a conservative heuristic
(`looks_encrypted` in `src/pager_events.*`):
- alpha pages need 8+ characters, at least 10% of them control codes other
  than LF/CR/HT;
- numeric pages need 16+ characters, at least 12% of them the BCD symbols
  `U [ ]`;
- FLEX `secure`/`binary` messages are flagged by type.

It catches ~90% of short and >99% of long random payloads. It flagged none
of 49 real plaintext pages, including symbol-heavy and pipe-delimited
dispatch text. The token is absent when the text looks readable; it is a
hint, not a guarantee.

FLEX control-channel broadcasts (BIW date/time/system id) arrive as
**`kind:"sync"`** — network information, like TETRA's `NETINFO1` — with
`extra` `protocol=flex; baud=…; info_type=biw_date|biw_time|biw_sysid|…` plus
that broadcast's own fields (e.g. `year`, `month`, `day`). FLEX per-frame BCH
statistics are not forwarded.

**Status:** end-to-end tested (`tests/test_session_pager.cpp`) against the
real multimon-ng: gen-ng synthetic POCSAG 512/1200/2400 and 1600 bps FLEX
pages, and multimon-ng's bundled **off-air** recordings — POCSAG at all three
rates and a 51 s capture of the Dutch P2000 FLEX network (46/46 pages) — each
FM-modulated into IQ and streamed through the server, which must return every
page a direct multimon-ng decode of the same audio finds. Not yet verified: a
live SDR as the source, and 3200/6400 bps 4-level FLEX (no capture or
generator available).

---

## Server → Client

Five frame shapes total: four JSON text frames (`capabilities`,
`started`, `error`, `event`) and one tagged binary frame (decoded
audio). Nothing else is ever sent.

### `capabilities` — what this build can emit

Sent **once, immediately on connect**, before the client sends anything.
It lets a client discover programmatically — without hard-coding this
table — which protocols this server build decodes, the event kinds it
emits, and the `extra` token keys it can produce. All values are `"; "`-
joined strings (keeping the flat-JSON, no-arrays invariant).

The set of `extra` keys depends on both the build (which decoder backend
was compiled in) and the protocol. At connect time the client hasn't
chosen a protocol yet, so the keys are grouped into per-protocol-family
fields (`extra_keys_dmr`, `extra_keys_p25`, …), each pre-filtered to the
keys **this build** can actually emit; a family this build can never emit
a key for is omitted entirely. The client reads the field for whichever
protocol it's about to request.

```json
{"type":"capabilities","protocols":"dmr; nxdn48; nxdn96; dpmr; dstar; ysf; p25; p25p2; provoice; edacs; edacs_esk; edacs_ea; edacs_ea_esk; x2tdma; tetra; tetrakit; pager-auto; pocsag; pocsag512; pocsag1200; pocsag2400; flex; auto","audio":"pcm_s16le_8000_mono","event_kinds":"voice; sync; call; message; burst; page; unknown","extra_keys_dmr":"network_type; network_id; site_id; rest_channel; lcn; svc; gps","extra_keys_p25":"rfss; site_id; system_id; wacn; alg_id; key_id; gps","extra_keys_nxdn":"site_code; system_code; location_id; category","extra_keys_dstar":"rpt1; rpt2; radio_text","extra_keys_ysf":"uplink; downlink; call_mode; data_type; src_rid; dst_rid","extra_keys_edacs":"lcn; afs; lid; system_id","extra_keys_tetra":"mcc; mnc; la; dlf; ulf; crypt; cid; nid; idx; status; afc; func; service; pdu; usage_marker; dl_usage_marker; encr","extra_keys_pager":"protocol; baud; message_type; function; flex_type; levels; phase; cycle; frame; addr_type; group; fragment; info_type; payload"}
```

| field | type | meaning |
|---|---|---|
| `type` | string | `"capabilities"` |
| `protocols` | string | `"; "`-joined `protocol` hint values this build actually decodes. |
| `audio` | string | Decoded-audio wire shape, currently always `"pcm_s16le_8000_mono"` (see the audio section below). |
| `event_kinds` | string | `"; "`-joined `event` `kind` values (`voice; sync; call; message; burst; page; unknown`). |
| `extra_keys_<family>` | string | `"; "`-joined `extra` token keys this build can emit for that protocol family (`dmr`, `p25`, `nxdn`, `dstar`, `ysf`, `edacs`, `tetra`, `pager`). Present only when non-empty. See the `extra` token vocabulary above for each token's meaning. |

The example above is from the dsd-fme build. The DSDcc build's frame is the
same shape but narrower: `protocols` drops the dsd-fme-only entries
(`p25`/`p25p2`/`provoice`/`edacs*`/`x2tdma`), `extra_keys_p25` /
`extra_keys_edacs` are absent, `extra_keys_dmr` is
`"unit_target; burst; sync_type"`, and `extra_keys_dstar` additionally
carries `gps`. A client keys off the advertised fields themselves rather
than needing to know which backend produced them.

A client that reads the first frame expecting `started` should first
consume (or skip past) this `capabilities` greeting — it is self-
identifying by its `type`.

### `started` — pipeline is up

Sent once per successful `start`, before any `event` or audio frame
from that pipeline (the reply is queued synchronously inside the
`start` handler; decoder callbacks are posted to the connection's
strand and therefore run after it).

```json
{"type":"started","udp_audio_port":44251}
```

| field | type | meaning |
|---|---|---|
| `type` | string | `"started"` |
| `udp_audio_port` | number | The **server-internal** UDP port this session's dsd-fme child streams decoded audio to (allocated per session from 40000–59000, collision-free across concurrent sessions). Purely informational/diagnostic — the client never talks to this port; audio arrives over the WebSocket. In the DSDcc backend build (`dsd-server-dsdcc`) there is no subprocess and no UDP, so this is `0`, meaning "not applicable", not "failed". |
| `iq_log_file` | string | Present **only** when the `start` had `iq_log:true` and the capture file opened. The server-side path of the MIDAS BLUE file this session's raw IQ is being written to. Absent when IQ logging was off or the file could not be opened. |

### IQ capture (`iq_log`)

When a `start` carries `iq_log:true`, the server tees the session's raw
IQ — the exact interleaved little-endian float32 (I0,Q0,I1,Q1,…) the
client sends as binary frames — to a **MIDAS BLUE** file (type 1000,
format `CF`, attached 512-byte header, `xdelta = 1/sample_rate`). The
bytes are written verbatim, so the capture is a bit-exact record of the
IQ the demod saw. The file is opened when the pipeline starts and
finalized (its `data_size` patched) on `stop` or disconnect; the path is
reported in the `started` reply's `iq_log_file` field.

Two environment variables tune it (server-side):

- `DSD_IQ_LOG_DIR` — directory the `.blue` files are written to (default
  the server's working directory). Created if missing.
- `DSD_IQ_LOG_MAX_MB` — per-session cap in mebibytes (default `1024`).
  On reaching it the capture stops on a sample boundary and a note is
  logged to stderr; the session itself keeps running.

Filenames are `iq_<YYYYMMDD_HHMMSS>_s<session>_<protocol>_<rate>Hz.blue`,
with `_c<centre>Hz` before `.blue` when the `start` carried a `center_freq`
(`tools/midas_ws_client.py` sends it again when replaying the file).
The files are readable by the repo's MIDAS tools
(`tools/midas_ws_client.py` and friends) and any BLUE-aware toolchain.
IQ logging is off by default; it writes a lot of data, so enable it only
when capturing a specific sample.

Besides the per-`start` `iq_log` flag, there is a **global switch** an
operator can flip live from the status page (the "Log IQ to BLUE file"
checkbox, see below) or over HTTP:

- `GET /iq_log/on` — start capturing on **every active session** (and any
  that start while it is on). Replies `{"iq_log_enabled":true}`.
- `GET /iq_log/off` — stop and finalize the capture on every session.
  Replies `{"iq_log_enabled":false}`.

The current switch state is also reported as `iq_logging` (boolean) in
`/status.json`. The switch is independent of the per-`start` flag: a
client can always request its own capture, and the operator switch is a
live override for whatever is running right now. Flipping it on opens a
capture file on each active session immediately (mid-stream); flipping it
off closes them (patching each file's `data_size`).

### `error` — something was rejected

The connection **stays open** after every error; only the offending
message is affected. Exactly nine message texts exist:

```json
{"type":"error","message":"unknown message type: <type>"}
{"type":"error","message":"bad control message: <parser detail>"}
{"type":"error","message":"binary frame length not a multiple of 8 bytes"}
{"type":"error","message":"invalid or missing key for key_type '<type>' (expected decimal/hex digits)"}
{"type":"error","message":"failed to start TETRA backend"}
{"type":"error","message":"failed to start DSD backend"}
{"type":"error","message":"failed to start pager backend"}
{"type":"error","message":"pager decoder exited unexpectedly"}
{"type":"error","message":"unknown pocsag_mode: <mode>"}
```

| message | trigger |
|---|---|
| `unknown message type: ...` | Text frame parsed as JSON but its `type` isn't one of the four control types. The unrecognized type is echoed after the colon. |
| `bad control message: ...` | Text frame that isn't valid flat JSON (or a field with an impossible value that throws in parsing). The detail after the colon is the parser's exception text — useful for debugging, not stable enough to match on programmatically. |
| `binary frame length not a multiple of 8 bytes` | Malformed binary IQ frame received after `start`. The frame is dropped. |
| `invalid or missing key for key_type '...' ...` | A `start` named a `key_type` (bp/rc4/des/aes/hytera/scrambler) but its `key` was empty or not decimal/hex digits. The key is validated before any backend is launched, so no pipeline starts; retry `start` with a valid key. |
| `failed to start TETRA backend` | A `start` with `protocol` `tetra`/`tetrakit` couldn't bring up the TETRA decoder subprocess (usually: no `tetra-rx` / `tetra-kit` on the server's PATH). No pipeline is running after this; a corrected `start` may be retried. |
| `failed to start DSD backend` | `start` couldn't bring up the decoder — for the subprocess backend, the fork/exec of dsd-fme failed (usually: no `dsd-fme` on the server's PATH); for the DSDcc backend, an unsupported config (e.g. a non-48 kHz internal rate). No pipeline is running after this; a corrected `start` may be retried. |
| `failed to start pager backend` | A paging `start` couldn't spawn multimon-ng (not on `PATH` / `$MULTIMON_NG`). No pipeline is running; a corrected `start` may be retried. |
| `pager decoder exited unexpectedly` | multimon-ng died mid-session — typically a too-old build (1.3.0) rejecting `--json`. Sent once; send `start` again once fixed. |
| `unknown pocsag_mode: ...` | A paging `start` with a `pocsag_mode` other than `auto`/`alpha`/`numeric`/`skyper`. No pipeline starts. |

Match on the prefix up to the first `:` if you need to branch on error
kind; treat the remainder as free text.

### `event` — decoder activity

One frame per line the DSD backend reports (subprocess backend: one per
line dsd-fme writes to its log, post-cleanup; DSDcc backend: one per
detected state change). All thirteen data fields are always present; empty
string means "not present in this event".

```json
{"type":"event","kind":"call","talkgroup":"19535","source_id":"2222223","slot":"2","color_code":"","ran":"","nac":"","emergency":"","alias":"","crc_error":"0","message":"","extra":"","raw":" SLOT 2 TGT=19535 SRC=2222223 Group Call  "}
```

| field | type | meaning |
|---|---|---|
| `type` | string | `"event"` |
| `kind` | string | Best-effort classification: `"voice"`, `"sync"`, `"call"`, `"message"` (a decoded DMR short-data/SMS body, dsd-fme backend), `"burst"` (DSDcc backend only), `"page"` (a decoded pager message — paging hints only, see the [Paging section](#paging-protocolpager-auto--pocsag--flex)), or `"unknown"`. See the per-backend notes below for exactly when each occurs. **On the subprocess backend, `unknown` events are suppressed by default** — dsd-fme prints a large startup banner / version / device-config block that all classifies as `unknown` noise, so it isn't forwarded (server-side `DsdProcessConfig::forward_unknown`; set it true to forward unrecognized lines for classifier debugging). Recognized events (`voice`/`sync`/`call`/`burst`) are always forwarded. |
| `talkgroup` | string | Decimal talkgroup / group-call target ID, or `""`. Kept as a string because IDs can exceed what a client might assume about integer width, and `""` is the natural "absent". |
| `source_id` | string | Decimal source radio ID, or `""`. |
| `slot` | string | TDMA slot, `"1"` or `"2"`, or `""` when the event isn't slot-specific. On the dsd-fme backend the physical slot is printed only on each burst's `Sync:` line (`[slot1]`/`[SLOT2]`); the server carries that slot forward onto the CSBK/call/voice/message lines dsd-fme prints for the *same* burst (they arrive with no marker of their own), so those events get the right `slot` without the client having to track it. Only traffic kinds (`voice`/`call`/`message`/`burst`) inherit it; channel-wide/`unknown` lines stay `""`, and any line that carries its own `[slotN]` overrides it. The DSDcc backend tags voice/call/burst events with their slot natively. |
| `color_code` | string | DMR color code as bare decimal (`"4"`, not `"04"` — both backends normalize away leading zeros), or `""` when the event doesn't carry one. Which event kinds carry it differs by backend: dsd-fme prints it on its per-burst sync lines, DSDcc's slot status text carries it on `voice`/`call` events. |
| `ran` | string | NXDN Radio Access Number as bare decimal (`"2"`, not `"02"`), or `""`. The NXDN analog of `color_code` — a repeater-access/filter code — surfaced by the dsd-fme backend on NXDN sync lines that carry `RAN NN`. DMR events leave it `""`, NXDN events leave `color_code` `""`. |
| `nac` | string | P25 Network Access Code as uppercase hex without `0x` (`"293"`), or `""`. The P25 analog of `color_code`/`ran`. dsd-fme backend only. |
| `emergency` | string | `"1"` when the line flags the call as an emergency (high-priority traffic), else `""`. dsd-fme backend only. |
| `alias` | string | DMR talker-alias text (the operator's over-the-air alias) when dsd-fme has assembled and printed it, else `""`. Free text. dsd-fme backend only. |
| `crc_error` | string | `"1"` when the decoder itself marked this line/burst as failing an FEC/CRC check, else `"0"`. This is the one flag reported as a definite `0`/`1` rather than `""`, so it always reads as a boolean; but `"0"` means "not flagged as an error", **not** "verified to have passed a CRC" — many lines carry no CRC status at all. Subprocess backend: `"1"` when the cleaned dsd-fme line carries a `CRC ERR`, `FEC ERR`, or `EMB ERR` marker. DSDcc backend: `"1"` on `burst` events whose slot-type PDU failed its Golay(20,8) FEC (`burst=UNK`). Treat a flagged event's other fields (especially `color_code`) as unreliable; note the reverse does not hold — a marginal burst can decode "cleanly" to a wrong value without being flagged. |
| `message` | string | Decoded **DMR short-data / SMS** text, else `""`. dsd-fme backend only: lifted from its `UTF8`/`UTF16`/`ISO7`/`ISO8 Text:` renderings of short-data (SDS) and UDT message PDUs. Free text — it may contain spaces, `;`, and `=`, which is why it has its own field rather than an `extra` token. Trailing decoder padding (`_` for null bytes, `-`/spaces for other non-printables) is trimmed. A UDT message shares its line with the sender/recipient, so `source_id`/`talkgroup` are set too; a short-data body arrives on its own line with just the text. **A body from a CRC/FEC-failed frame is dropped** — dsd-fme prints its garbage decode with a `(CRC ERR)`/`(FEC ERR)` marker; the event still carries `crc_error:"1"`, but `message` stays `""` rather than surfacing untrustworthy text. Encrypted or segmented (multi-block) messages may arrive partial or garbled; the codec cannot recover encrypted bodies. DSDcc backend does not decode the DMR data plane, so it never sets this. |
| `extra` | string | Backend-specific detail that doesn't fit the fields above, or `""`. **Format: zero or more `key=value` tokens joined by `"; "`.** DSDcc backend tokens: `unit_target=<id>` (unit-to-unit call target — not a talkgroup, so kept out of `talkgroup`), `burst=<type>` on `burst` events (three-letter slot burst type: `IDL` idle, `CSB` CSBK control, `VLC`/`TLC` voice/terminator link control, `VOX` voice, `UNK` unknown, …), `sync_type=<flavor>` on sync-acquisition events (see below). dsd-fme backend NXDN trunking tokens: `site_code=<n>`, `system_code=<n>`, `location_id=<hex>`, `category=<name>` (e.g. `Global`). Because the same label can mean different things per line (a `Site Code` is the home site on a Site ID line but an adjacent site on an Adjacent Information line), read the accompanying `raw` for context. |
| `raw` | string | The underlying decoder output this event was parsed from, so nothing is lost to the classification: the cleaned log line (subprocess backend) or a synthesized description (DSDcc backend). Free text; formats below are examples from real decodes, and they **vary across dsd-fme versions/forks** — parse the structured fields, fall back to `raw` only for display/debugging. |

#### Which fields each protocol populates

Every field is **always present** in every `event` frame regardless of
protocol; this table says which ones ever carry a non-`""` value for a
given protocol (driven by the `protocol` start hint). A blank cell means
the field stays `""` for that protocol — not that it is omitted.

| field | DMR | NXDN | P25 | dPMR | D-STAR | YSF | TETRA | notes |
|---|:---:|:---:|:---:|:---:|:---:|:---:|:---:|---|
| `type` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | always `"event"` |
| `kind` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | `burst` is DSDcc/DMR-only; NXDN/P25 control messages are `unknown`. TETRA: `sync`/`call`/`unknown` (osmo), plus `voice` on the tetra-kit backend |
| `talkgroup` | ✓ | ✓ | ✓ | ✓ | ‡ | ‡ | † | DMR `TGT=`; NXDN `Dst/TG=`; P25 `TGT:`; dPMR `TG=` (zero-padding stripped). ‡ D-STAR/YSF: a **callsign** (the destination — e.g. `CQCQCQ`), not a numeric id. † TETRA: the called group SSI on call-control PDUs — **osmo backend only** (tetra-kit associates the caller SSI later by usage marker and leaves this `""`) |
| `source_id` | ✓ | ✓ | ✓ | ✓ | ‡ | ‡ | ✓ | `Src=` / `SRC:` (zero-padding stripped). ‡ D-STAR/YSF: the transmitting station's **callsign**. TETRA: the party SSI |
| `slot` | ✓ | — | — | — | — | — | — | DMR TDMA slot `1`/`2` |
| `color_code` | ✓ | — | — | ✓* | — | — | † | DMR color code / dPMR channel code; *dPMR: dsd-fme backend only. † TETRA: the network **colour code** (`NETINFO1 CCODE`) — **osmo backend only** |
| `ran` | — | ✓ | — | — | — | — | — | NXDN Radio Access Number |
| `nac` | — | — | ✓* | — | — | — | — | P25 Network Access Code (hex); *dsd-fme backend only |
| `emergency` | ✓* | ✓* | ✓* | ✓* | — | — | — | emergency flag; *dsd-fme backend only |
| `alias` | ✓* | — | — | — | — | — | — | DMR talker alias; *dsd-fme backend only |
| `crc_error` | ✓ | ✓ | ✓ | ✓ | ✓* | ✓* | — | FEC/CRC-failure flag (dsd-fme, and DSDcc for DMR/NXDN); *D-STAR/YSF: dsd-fme only. TETRA: the external decoder discards failing PDUs itself, so surviving events aren't CRC-flagged |
| `message` | ✓* | — | — | — | — | — | — | DMR short-data / SMS text; *dsd-fme backend only (DSDcc doesn't decode the DMR data plane) |

Paging (not a column above): `talkgroup` (capcode), `message` (page text),
`emergency` (FLEX priority), `crc_error` (FLEX checksum), `extra` and `raw`
carry values; every other field stays `""`. See the
[Paging section](#paging-protocolpager-auto--pocsag--flex).
| `extra` | ✓ | ✓ | ✓ | — | ✓ | ✓ | ✓ | protocol/backend-specific `key=value` tokens (see below) |
| `raw` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | always the source line/description |

(`color_code`/`ran`/`nac` are the per-protocol access codes — DMR & dPMR,
NXDN, P25 respectively — so at most one is populated for a given signal.
D-STAR and YSF are amateur protocols that identify stations by **callsign**,
so none of the numeric access codes apply; `source_id`/`talkgroup` carry
callsign text instead of numbers, and the repeater/routing detail rides in
`extra`. TETRA reuses `color_code` for its colour code, but only the osmo
backend surfaces it; the network identity — MCC/MNC/LA and the up/downlink
frequencies — rides in `extra` rather than in dedicated fields.)

`extra` token vocabulary by protocol/backend:

| token | protocol | backend | meaning |
|---|---|---|---|
| `unit_target=<id>` | DMR | DSDcc | private-call target (not a talkgroup) |
| `burst=<type>` | DMR | DSDcc | slot burst type (`IDL`/`CSB`/`VLC`/`TLC`/`VOX`/`UNK`) |
| `sync_type=<flavor>` | DMR | DSDcc | `dmr_bs_data` / `dmr_ms_voice` / … |
| `network_type=<con+\|cap+>` | DMR | dsd-fme | Motorola trunking flavor (Connect Plus / Capacity Plus) |
| `network_id=<n>` | DMR | dsd-fme | trunked network ID (Tier III / Con+ / Cap+) |
| `site_id=<n>` | DMR | dsd-fme | trunked site ID (may be `N.M` form) |
| `rest_channel=<n>` | DMR | dsd-fme | rest channel / rest LSN |
| `lcn=<n>` | DMR | dsd-fme | logical channel number (`LCN`/`LPCN`) |
| `svc=<service>` | DMR | dsd-fme | what a data-call line carries: `preamble` (data preamble CSBK), `ack` (Response Packet header), `data` (Confirmed / Unconfirmed Delivery header), or the Motorola MNIS service after it: `ars`, `lrrp`, `tms`, … or `mnis:<hex type>` when dsd-fme doesn't name it |
| `gps=<lat>,<lon>` | DMR/P25 | dsd-fme | a position report in signed decimal degrees (5 places), e.g. `gps=39.03494,-76.98460` -- DMR LRRP / GPS, P25 Motorola LCW GPS |
| `rfss=<n>` | P25 | dsd-fme | RF Sub-System id (trunking) |
| `site_id=<n>` | DMR/P25 | dsd-fme | site id (DMR `Site ID:`, P25 `Site:`/`SITE [ ]`) |
| `system_id=<hex>` | P25 | dsd-fme | P25 System ID |
| `wacn=<hex>` | P25 | dsd-fme | Wide Area Communications Network id |
| `alg_id=<hex>` | P25 | dsd-fme | encryption algorithm id (`80`=clear, `84`=AES256, `AA`=ADP, …) |
| `key_id=<hex>` | P25 | dsd-fme | encryption key id (`0000`=unencrypted) |
| `site_code=<n>` | NXDN | both | site code (home or adjacent — see `raw`) |
| `system_code=<n>` | NXDN | both | trunked system code |
| `location_id=<hex>` | NXDN | both | site location ID |
| `category=<name>` | NXDN | dsd-fme | system scope, e.g. `Global` |
| `rpt1=<call>` | D-STAR | both | uplink repeater callsign (RPT1) |
| `rpt2=<call>` | D-STAR | both | gateway/link repeater callsign (RPT2) |
| `radio_text=<text>` | D-STAR | both | slow-data message text (free text) |
| `gps=<locator>` | D-STAR | DSDcc | Maidenhead locator from slow-data GPS |
| `uplink=<call>` | YSF | both | repeater uplink callsign (U/L) |
| `downlink=<call>` | YSF | both | repeater downlink callsign (D/L) |
| `call_mode=<mode>` | YSF | both | FICH call mode: `group_cq` / `radio_id` / `individual` |
| `data_type=<type>` | YSF | both | FICH data type: `vd1` / `vd2` / `voice_full` / `data_full` |
| `src_rid=<n>` / `dst_rid=<n>` | YSF | both | numeric DSQ radio IDs (Radio ID call mode) |
| `lcn=<n>` | EDACS/ProVoice | dsd-fme | logical channel number (control or working channel) |
| `afs=<n>` | EDACS/ProVoice | dsd-fme | EDACS Agency-Fleet-Subfleet group id (decimal) |
| `lid=<n>` | EDACS/ProVoice | dsd-fme | EDACS logical (unit) id, on login/regroup lines |
| `system_id=<hex>` | EDACS/ProVoice | dsd-fme | EDACS system id from the control channel |
| `mcc=<hex>` / `mnc=<hex>` | TETRA | osmo | Mobile Country / Network Code, hex as the fork emits (e.g. `00ea`=234, `004e`=78) |
| `la=<n>` | TETRA | osmo | Location Area |
| `dlf=<hz>` / `ulf=<hz>` | TETRA | osmo | down/uplink carrier frequency in Hz |
| `crypt=<n>` | TETRA | osmo | air-interface encryption flag from `NETINFO1` |
| `cid=<n>` / `nid=<n>` | TETRA | osmo | cell id / network id on call-control PDUs |
| `idx=<n>` | TETRA | osmo | call index — associates later voice with this call |
| `status=<n>` / `afc=<n>` / `func=<name>` | TETRA | osmo | short-data status; AFC diagnostic; raw TETMON `FUNC` name (mostly on `unknown` events) |
| `service=<name>` / `pdu=<name>` | TETRA | tetra-kit | tetra-kit report's service (`UPLANE`/`MLE`/…) and PDU type (`TCH_S`/`MAC-SYNC`/…) |
| `usage_marker=<n>` / `dl_usage_marker=<n>` | TETRA | tetra-kit | uplink / downlink usage marker — associates speech frames with a call |
| `encr=<mode>` | TETRA | tetra-kit | encryption mode reported for the PDU |

Both backends emit the NXDN fields (`ran`, `source_id`, `talkgroup`, and
the `site_code`/`system_code`/`location_id` tokens) with the same shape,
so a client sees a consistent structure regardless of which backend
decoded. The DSDcc backend derives `system_code`/`site_code` from the
high/low 12 bits of its decoded location ID (the same split dsd-fme
prints); it does not currently surface `category`.

The DMR trunking / LC fields (`emergency`, `alias`, and the
`network_type` / `network_id` / `site_id` / `rest_channel` / `lcn` / `svc` / `gps`
tokens) are **dsd-fme backend only**: they come from DMR CSBK, data, and
talker-alias layers that the DSDcc backend does not decode (DSDcc's DMR
decoder handles voice, slot type / color code, and source/target from
the embedded LC, but not the CSBK payload). On the DSDcc backend these
stay `""`. Because the project has no Con+/Cap+/Tier-III capture to drive
them, these patterns are verified against lwvmobile/dsd-fme's own printf
formats (pinned in `tests/test_dsd_fme_parse.cpp`) rather than a live
decode; like all `raw`-derived parsing they may vary across dsd-fme
versions.

**P25** (`nac`, `rfss`/`site_id`/`system_id`/`wacn`, `alg_id`/`key_id`,
plus the shared `talkgroup`/`source_id`/`emergency`) is **dsd-fme backend
only**. dsd-fme has full P25 Phase 1/2 + trunking decode; the DSDcc
backend has a P25 Phase 1 *decoder* but this wrapper does not yet read
its metadata, so on the DSDcc backend a P25 stream produces sync events
only and these fields stay `""`. The P25 patterns are verified against a
real P25 Phase 1 control-channel capture (`nac`, `rfss`, `site_id`,
`system_id`, `wacn` all confirmed live end to end); the encryption
`alg_id`/`key_id` and `emergency` shapes, which that capture did not
exercise, are pinned against dsd-fme's source formats in
`tests/test_dsd_fme_parse.cpp`.

Protocol vs. backend: **DMR** decodes on both the dsd-fme and DSDcc
backends. **NXDN** parses on both, but is only *reliable* on the dsd-fme
backend — the DSDcc backend's NXDN symbol recovery drops sync on real
off-air signals (it decodes clean/synthetic input fine), so prefer
dsd-fme for real NXDN (see the `protocol` field note above). **P25** is
dsd-fme only (above). **dPMR** decodes on both backends (both verified
against DSDcc's bundled `samples/dpmr.dis`): the dsd-fme backend reports
`talkgroup`/`source_id`/`color_code` (the dPMR channel code, from
`Channel Code=NN`), the DSDcc backend reports `talkgroup`/`source_id`
from its own/called ids (its channel-code accessor is unreliable, so it
omits `color_code`). As with DMR, the two backends can report different
ids for the same call (they read different frame fields).

**D-STAR** and **YSF** decode on both backends (both verified live against
DSDcc's bundled `samples/dstar_f1zil_1.dis` and `samples/ysf_f5zoo.dis`,
and the dsd-fme parsing pinned against real lines from the same captures
in `tests/test_dsd_fme_parse.cpp`). These are amateur protocols keyed on
**callsigns**, so `source_id`/`talkgroup` carry callsign text and the
numeric access codes stay `""`. D-STAR surfaces the transmitting callsign
(`source_id`), the "your call" destination (`talkgroup`, e.g. `CQCQCQ`),
the repeater path (`rpt1`/`rpt2`), slow-data `radio_text`, and — DSDcc
only — a Maidenhead `gps` locator. YSF surfaces `source_id`/`talkgroup`
callsigns, the repeater `uplink`/`downlink`, and the FICH `call_mode` /
`data_type`; in Radio-ID call mode the numeric `src_rid`/`dst_rid` DSQ ids
appear instead of callsigns. Two backend-shape differences worth noting:
the DSDcc backend emits one consolidated `call` event per state change,
while dsd-fme emits a separate event per frame (source on one, repeater on
the next, …) because its metadata arrives on separate log lines; and the
two decoders can disagree on a repeater's module letter or a partly-copied
callsign, the same cross-backend caveat as the numeric protocols.

Anything else auto-detects into `raw` under `kind:"unknown"`.

Encoding guarantee: strings are JSON-escaped per RFC 8259 including
control characters (`\u00XX`), and the subprocess backend strips ANSI
color sequences and carriage returns from dsd-fme's output before
parsing — so `raw` is clean printable text and every frame is valid
JSON even though dsd-fme colorizes its terminal log.

#### Subprocess backend (`dsd-server`, real dsd-fme) — real examples

Captured from lwvmobile/dsd-fme decoding a real DMR group call
(the test suite's `session_real_fme_test`):

```json
{"type":"event","kind":"sync","talkgroup":"","source_id":"","slot":"2","color_code":"4","ran":"","nac":"","emergency":"","alias":"","crc_error":"0","message":"","extra":"","raw":"20:37:20 Sync: +DMR   slot1  [SLOT2] | Color Code=04 | VC6 "}
{"type":"event","kind":"call","talkgroup":"19535","source_id":"2222223","slot":"2","color_code":"","ran":"","nac":"","emergency":"","alias":"","crc_error":"0","message":"","extra":"","raw":" SLOT 2 TGT=19535 SRC=2222223 Group Call  "}
{"type":"event","kind":"sync","talkgroup":"","source_id":"","slot":"1","color_code":"5","ran":"","nac":"","emergency":"","alias":"","crc_error":"1","message":"","extra":"","raw":"13:37:43 Sync: +DMR  [slot1]  slot2  | Color Code=05 | MBCC (FEC ERR)"}
```

(dsd-fme's `unknown`-kind lines — its startup banner, `Decoding DMR BS/MS
Simplex`, and the like — are suppressed by default, so they don't appear
in this stream; see the `kind` notes below.)

- `kind:"sync"` — any line containing "Sync" (dsd-fme's per-burst sync
  lines, several per second while a signal is present). `slot` is taken
  from the **bracketed** slot marker (`[SLOT2]` above) — the slot the
  current burst belongs to.
- `kind:"call"` — a line carrying talkgroup/source IDs without
  voice/sync keywords (dsd-fme's `TGT=... SRC=...` call summary lines).
- `kind:"voice"` — a line containing "voice" or "ambe" (e.g. dsd-fme's
  `VOICE CACH/EMB ERR`).
- `crc_error:"1"` rides on any of the above whose line dsd-fme marked
  as a failed FEC/CRC check. The flagged example above is real: the
  reference capture's only wrong Color Code (5 instead of 4, one
  burst out of 660) sits on a line marked `(FEC ERR)` — filtering on
  this flag removes it.
- `kind:"unknown"` — everything else dsd-fme prints: the startup banner /
  version / device-config block, `Activity Update ...` summaries, the exit
  summary, and any line the classifier didn't match. **These are
  suppressed by default** (`DsdProcessConfig::forward_unknown`), since the
  startup block in particular is pure boilerplate; set `forward_unknown`
  true to forward them (the `raw` field carries the original line, useful
  when extending the classifier).

##### DMR trunking / talker alias / emergency (dsd-fme backend)

Shapes from dsd-fme's Con+/Cap+/Tier-III and LC output (source-format
verified — see the backend note above). `raw` is illustrative:

```json
{"type":"event","kind":"call","talkgroup":"","source_id":"2048","slot":"","color_code":"","ran":"","nac":"","emergency":"","alias":"","crc_error":"0","message":"","extra":"network_type=con+; lcn=3","raw":" Connect Plus Group Voice Channel Grant; Target: 100; Source: 2048; LCN: 3; TS: 1;"}
{"type":"event","kind":"unknown","talkgroup":"","source_id":"","slot":"","color_code":"","ran":"","nac":"","emergency":"","alias":"","crc_error":"0","message":"","extra":"network_type=cap+; rest_channel=5","raw":" Capacity Plus Channel Status - FL: 1 TS: 1 RS: 0 - Rest LSN: 5"}
{"type":"event","kind":"unknown","talkgroup":"","source_id":"","slot":"","color_code":"","ran":"","nac":"","emergency":"","alias":"","crc_error":"0","message":"","extra":"network_id=9; site_id=1","raw":" C_ALOHA_SYS_PARMS: Tier III; Net ID: 9; Site ID: 1;"}
{"type":"event","kind":"call","talkgroup":"","source_id":"2048","slot":"","color_code":"","ran":"","nac":"","emergency":"","alias":"JOHN SMITH","crc_error":"0","message":"","extra":"","raw":" TG: 100; SRC: 2048; Talker Alias: JOHN SMITH"}
{"type":"event","kind":"call","talkgroup":"100","source_id":"2048","slot":"1","color_code":"","ran":"","nac":"","emergency":"1","alias":"","crc_error":"0","message":"","extra":"","raw":" SLOT 1 TGT=100 SRC=2048 Group Emergency"}
```

- `emergency:"1"` flags a line carrying an emergency marker (the value
  forms `Emergency: <timer>` / `Emergency = <n>` do **not** trip it).
- `alias` is the talker-alias text once dsd-fme assembles it.
- `network_type`/`network_id`/`site_id`/`rest_channel`/`lcn` ride in
  `extra`; DSDcc leaves all of these empty (it doesn't decode CSBK).

##### DMR short data / SMS (dsd-fme backend)

DMR CSBK and data-header lines name the destination `Target: N` rather than
`TGT`/`TG` (`Preamble CSBK - Group Data - Source: 123 - Target: 1`, `Slot 1
Data Header - Group - Unconfirmed Delivery - Source: 123 Target: 1`); that
value fills `talkgroup` (the destination id -- a talkgroup, or a radio for an
individual data call) when no `TGT`/`TG` is on the line.

Text-message decode from dsd-fme's short-data (SDS) and UDT PDU output.
Shapes are pinned against dsd-fme's own `…Text:` render formats
(`dmr_pdu.c` / `dmr_block.c` — source-format verified, no SMS capture in the
tree to exercise them live), so `raw` is representative:

```json
{"type":"event","kind":"message","talkgroup":"12345","source_id":"6789","slot":"1","color_code":"","ran":"","nac":"","emergency":"","alias":"","crc_error":"0","message":"HELLO WORLD","extra":"","raw":"Slot 1 - SRC: 6789; TGT: 12345; UDT ISO7 Text: HELLO WORLD"}
{"type":"event","kind":"message","talkgroup":"","source_id":"","slot":"","color_code":"","ran":"","nac":"","emergency":"","alias":"","crc_error":"0","message":"on my way, eta 10 min","extra":"","raw":" UTF8 Text: on my way, eta 10 min"}
```

- The first is a **UDT** message: sender/recipient and body decode on one
  line, so `source_id`/`talkgroup` are filled alongside `message`. The second
  is **short data**, whose body arrives on its own line (the address came
  earlier), so only `message` is set.
- `kind` is `"message"` — a recognized kind, so these are forwarded even
  though they carry no voice/call metadata (they would otherwise fall to
  `unknown` and be suppressed).
- The body is free text: it may contain spaces, `;`, `=` — hence its own
  field, never an `extra` token. dsd-fme's padding (`_` for null bytes,
  `-`/spaces for other non-printables) is trimmed off the tail. Encrypted or
  multi-block/segmented messages may come through partial or garbled.

There is a **third** path for IP/UDP-based TMS short data (Motorola/Hytera-style
SMS), which dsd-fme only decodes with its `-Z` payload-logging flag — the server
passes it by default. Instead of a `…Text:` line, the message text rides inside
the reassembled *Multi Block PDU* the `-Z` dump prints; the server reassembles
that PDU's bytes and extracts the embedded text (UTF-16LE or ASCII/UTF-8),
emitting it as a normal `message` event:

```json
{"type":"event","kind":"message","talkgroup":"","source_id":"","slot":"1","color_code":"","ran":"","nac":"","emergency":"","alias":"","crc_error":"0","message":"test","extra":"","raw":"DMR short-data PDU (dsd-fme -Z): test"}
```

- This is a **real** capture: an SMS "test" sent to a talkgroup, decoded from
  its UTF-16LE TMS payload. `slot` is filled from the PDU's slot; the TMS
  transport doesn't restate the SMS source/target on the text line, so those
  stay `""` (they appear on the preceding data-header/preamble `call` events).
- A message is emitted **only** when the reassembled PDU actually contains a
  printable text run. Binary/control/telemetry PDUs (and every per-frame `-Z`
  payload line) decode to no text and are silently dropped, so this adds no
  new event noise — voice-only traffic produces no `message` events at all.
- Disable the whole path (and the `-Z` flag) by building/running with the
  subprocess backend's `decode_short_data` off; it is on by default.

##### P25 (subprocess backend with `protocol:"p25"` / `"p25p2"`)

The first two frames are **real** — from a P25 Phase 1 control-channel
capture; the rest (a voice call with encryption) are source-format
shapes:

```json
{"type":"event","kind":"sync","talkgroup":"","source_id":"","slot":"","color_code":"","ran":"","nac":"717","emergency":"","alias":"","crc_error":"0","message":"","extra":"rfss=1; site_id=97","raw":"17:30:46 Sync: +P25p1 NAC/CC: 717; RFSS: 001; Site: 097;  TSBK"}
{"type":"event","kind":"unknown","talkgroup":"","source_id":"","slot":"","color_code":"","ran":"","nac":"","emergency":"","alias":"","crc_error":"0","message":"","extra":"site_id=91; rfss=1; system_id=715","raw":" LRA [00] CFVA [3] RFSS[001] SITE [091] SYSID [715]"}
{"type":"event","kind":"call","talkgroup":"100","source_id":"2048","slot":"","color_code":"","ran":"","nac":"293","emergency":"","alias":"","crc_error":"0","message":"","extra":"","raw":"P25 TGT: 00000100; SRC: 00002048; NAC: 293;"}
{"type":"event","kind":"unknown","talkgroup":"","source_id":"","slot":"","color_code":"","ran":"","nac":"","emergency":"","alias":"","crc_error":"0","message":"","extra":"alg_id=84; key_id=0001","raw":" HDU  ALG ID: 0x84 KEY ID: 0x0001 MI: 0x0123456789ABCDEF"}
```

- `nac` (hex) is the P25 network access code — the P25 analog of DMR
  `color_code` and NXDN `ran`.
- `rfss` / `site_id` / `system_id` / `wacn` are the P25 trunking system
  identity, decoded from control-channel broadcasts (both dsd-fme's
  `RFSS: 001`/`Site: 097` and `RFSS[001]`/`SITE [091]`/`WACN [BEE0A]`
  forms). As on NXDN, the same label can be home vs. adjacent — read
  `raw`.
- `alg_id=80`/`key_id=0000` means clear/unencrypted; `alg_id=84` AES256,
  `alg_id=aa` Motorola ADP, etc. (values as dsd-fme reports).
- Zero-padded IDs (`TGT: 00000100`) are normalized (`talkgroup:"100"`).

##### dPMR (`protocol:"dpmr"`) — real, both backends

Real frames from decoding `samples/dpmr.dis`. dsd-fme reports the dPMR
channel code in `color_code`; both backends report talkgroup/source
(from different frame fields, so the ids can differ):

```json
{"type":"event","kind":"call","talkgroup":"10011","source_id":"243","slot":"","color_code":"31","ran":"","nac":"","emergency":"","alias":"","crc_error":"0","message":"","extra":"","raw":" TG=0010011 Src=0000243 Channel Code=31"}
{"type":"event","kind":"unknown","talkgroup":"","source_id":"","slot":"","color_code":"","ran":"","nac":"","emergency":"","alias":"","crc_error":"1","message":"","extra":"","raw":" TG=(CRC ERR) Src=(CRC ERR) Channel Code =(CRC ERR)"}
{"type":"event","kind":"call","talkgroup":"14653","source_id":"302","slot":"","color_code":"","ran":"","nac":"","emergency":"","alias":"","crc_error":"0","message":"","extra":"","raw":"(dsdcc dpmr) own 302 called 14653"}
```

The first two are the dsd-fme backend (note `color_code:"31"` and the
CRC-flagged bad frame); the third is the DSDcc backend (own/called ids,
no channel code). Both are from the same file — the id disagreement
(10011/243 vs 14653/302) is the usual cross-backend decode-layer
difference.

##### D-STAR (`protocol:"dstar"`) — real, both backends

Real frames from decoding `samples/dstar_f1zil_1.dis` (F1NSR calling CQ
through the F1ZIL repeater). `source_id`/`talkgroup` carry **callsigns**;
the numeric access codes stay `""`:

```json
{"type":"event","kind":"call","talkgroup":"CQCQCQ","source_id":"F1NSR ID51","slot":"","color_code":"","ran":"","nac":"","emergency":"","alias":"","crc_error":"0","message":"","extra":"rpt1=F1ZIL B; rpt2=F1ZIL G","raw":"18:27:55 Sync: -DSTAR VOICE   RPT 2: F1ZIL  G RPT 1: F1ZIL  B DST: CQCQCQ   SRC: F1NSR   ID51 REPEATER"}
{"type":"event","kind":"call","talkgroup":"CQCQCQ","source_id":"F1NSR /ID51","slot":"","color_code":"","ran":"","nac":"","emergency":"","alias":"","crc_error":"0","message":"","extra":"rpt1=F1ZIL B; rpt2=F1ZIL B; radio_text=YANNICK ST RAPHAEL","raw":"(dsdcc dstar) my F1NSR /ID51 ur CQCQCQ"}
```

The first is the dsd-fme backend (the call info shares its reprinted sync
line); the second is the DSDcc backend, which consolidates the callsigns,
repeater path, and the assembled slow-data `radio_text` into one event.
(The two decoders copy the repeater module letter differently — `F1ZIL G`
vs `F1ZIL B` — the same cross-backend caveat as the numeric protocols.)

##### YSF / System Fusion (`protocol:"ysf"`) — real, both backends

Real frames from decoding `samples/ysf_f5zoo.dis` (F1SER/F6FCE via the
F5ZOO-R1 repeater, group CQ, V/D type 2). dsd-fme's metadata arrives on
separate frame lines (source, then uplink/downlink), so it emits one event
each; the DSDcc backend consolidates:

```json
{"type":"event","kind":"call","talkgroup":"","source_id":"F1SER","slot":"","color_code":"","ran":"","nac":"","emergency":"","alias":"","crc_error":"0","message":"","extra":"call_mode=group_cq; data_type=vd2","raw":"18:28:18 Sync: +YSF  V/D2 Group/CQ -Simplex CC FN: 2/7 SRC: F1SER     "}
{"type":"event","kind":"call","talkgroup":"","source_id":"","slot":"","color_code":"","ran":"","nac":"","emergency":"","alias":"","crc_error":"0","message":"","extra":"call_mode=group_cq; data_type=vd2; uplink=F5ZOO-R1","raw":"18:28:18 Sync: +YSF  V/D2 Group/CQ -Simplex CC FN: 3/7 U/L: F5ZOO-R1  "}
{"type":"event","kind":"call","talkgroup":"","source_id":"F1SER","slot":"","color_code":"","ran":"","nac":"","emergency":"","alias":"","crc_error":"0","message":"","extra":"call_mode=group_cq; data_type=vd2; uplink=F5ZOO-R1; downlink=F5ZOO-R1","raw":"(dsdcc ysf) src F1SER dst "}
```

The first two are the dsd-fme backend (source on one frame, uplink on the
next); the third is the DSDcc backend, carrying source, repeater
uplink/downlink, and the FICH call mode / data type together. The masked
group-CQ destination (`**********`) is normalized to an empty `talkgroup`
rather than an invented id.

##### NXDN / IDAS (subprocess backend with `protocol:"nxdn48"`)

Captured from dsd-fme decoding a real off-air NXDN48/IDAS trunked
control channel (the same signal used to verify the `protocol` hint):

```json
{"type":"event","kind":"voice","talkgroup":"","source_id":"","slot":"","color_code":"","ran":"2","nac":"","emergency":"","alias":"","crc_error":"0","message":"","extra":"","raw":"Sync: NXDN48  RTCH Voice  RAN 02 PF X/4"}
{"type":"event","kind":"call","talkgroup":"2043","source_id":"958","slot":"","color_code":"","ran":"","nac":"","emergency":"","alias":"","crc_error":"0","message":"","extra":"","raw":" Session Call - ... - Src=958 - Dst/TG=2043 - Prefix Ch: 3 "}
{"type":"event","kind":"unknown","talkgroup":"","source_id":"","slot":"","color_code":"","ran":"","nac":"","emergency":"","alias":"","crc_error":"0","message":"","extra":"site_code=1","raw":"Site ID Message - Area: 0; Site Type: 8 Narrow; Site Code: 1 Open Access;  FACCH3"}
{"type":"event","kind":"unknown","talkgroup":"","source_id":"","slot":"","color_code":"","ran":"","nac":"","emergency":"","alias":"","crc_error":"0","message":"","extra":"site_code=2; system_code=8; category=Global","raw":"Adjacent Information - Cat: Global - Sys Code: 8 - Site Code 2 "}
{"type":"event","kind":"unknown","talkgroup":"","source_id":"","slot":"","color_code":"","ran":"","nac":"","emergency":"","alias":"","crc_error":"0","message":"","extra":"location_id=008002","raw":"Service Information - Location ID [008002] SVC [01A8] RST [000000] "}
```

- `ran` carries the NXDN Radio Access Number from `RTCH ... RAN NN`
  sync lines. `source_id`/`talkgroup` come from `Src=`/`Dst/TG=` (and
  `TGT:` on channel-update lines) exactly as for DMR.
- The trunking-identity messages (Site ID, Adjacent/Location
  Information, Service Information) carry no call IDs, so they stay
  `kind:"unknown"` with their structured detail in `extra` and the full
  text in `raw`.
- Reminder (see the `protocol` field above): this is the **dsd-fme
  backend**, which decodes NXDN reliably. The DSDcc backend emits the
  same NXDN fields (see its section below) but only decodes NXDN on
  clean signals, not real off-air captures.

#### DSDcc backend (`dsd-server-dsdcc`) — real examples

Captured from DSDcc 1.9.0 decoding the same call
(`session_dsdcc_test`):

```json
{"type":"event","kind":"sync","talkgroup":"","source_id":"","slot":"","color_code":"","ran":"","nac":"","emergency":"","alias":"","crc_error":"0","message":"","extra":"sync_type=dmr_bs_data","raw":"(dsdcc: sync acquired, dmr_bs_data)"}
{"type":"event","kind":"burst","talkgroup":"","source_id":"","slot":"1","color_code":"4","ran":"","nac":"","emergency":"","alias":"","crc_error":"0","message":"","extra":"burst=IDL","raw":"(dsdcc slot1) .04 IDL                   "}
{"type":"event","kind":"voice","talkgroup":"150607","source_id":"2222223","slot":"2","color_code":"4","ran":"","nac":"","emergency":"","alias":"","crc_error":"0","message":"","extra":"","raw":"(dsdcc slot2) *04 VOX 02222223>G00150607"}
```

- `kind:"sync"` — sync acquisition/loss transitions only, not
  per-burst like dsd-fme — expect far fewer of these. On acquisition,
  `extra` carries the sync flavor as `"sync_type=<flavor>"`, the DSDcc
  equivalent of dsd-fme's `+DMR MS/DM` detail: `dmr_bs_data`,
  `dmr_bs_voice` (base station / repeater), `dmr_ms_data`,
  `dmr_ms_voice` (mobile station — also what direct/simplex mode
  shows), `other` (non-DMR sync in auto mode). Only the acquiring
  burst's flavor is reported; within a held sync the flavor alternates
  per burst (voice on one slot, data on the other) and is deliberately
  not re-reported. On loss, `extra` is `""` and `raw` is
  `"(dsdcc: sync lost)"`.
- `kind:"voice"` / `kind:"call"` — a change in a slot's call state,
  `voice` while that slot's voice channel is active, `call` otherwise.
  `raw` is `"(dsdcc slot<n>) "` followed by DSDcc's 26-character slot
  status text (activity flag, color code, burst type, `source>G|Utarget`).
- `kind:"burst"` — a change in a slot's burst type or color code
  **before/without call addresses** (DSDcc only learns addresses from
  voice embedded signalling). `extra` is `"burst=<type>"`, and
  `color_code` is filled once the slot-type PDU decodes. This is all
  the visibility DSDcc has into control-only traffic — e.g. a capture
  of CSBK signalling produces `burst=CSB` events with the color code,
  where dsd-fme would additionally decode the CSBK payload (source /
  target / opcode). Idle slots show as `burst=IDL`.
- For **unit-to-unit** (private) calls, `talkgroup` stays `""` and the
  target lands in `extra` as `"unit_target=<id>"`.
- `kind:"unknown"` is not currently produced by this backend.

In NXDN mode (`protocol:"nxdn48"`/`"nxdn96"`) this backend emits the same
NXDN fields as the dsd-fme backend, read from DSDcc's NXDN decoder — a
`kind:"call"` event carrying `ran`, `source_id`, `talkgroup` (group
target) or `extra: unit_target=<id>` (private), and, on site messages,
`extra: system_code=<n>; site_code=<n>; location_id=<hex>`:

```json
{"type":"event","kind":"call","talkgroup":"200","source_id":"100","slot":"","color_code":"","ran":"9","nac":"","emergency":"","alias":"","crc_error":"0","message":"","extra":"","raw":"(dsdcc nxdn) RAN 9 src 100 dst 200 group"}
```

Remember the reliability caveat: DSDcc decodes NXDN only on clean signals
(the example is from the repo's synthetic sample); on real off-air NXDN
it drops sync where dsd-fme succeeds.

Note the two backends can legitimately disagree on metadata for the
same signal — they decode different link-control layers (for the
bundled test capture, dsd-fme reports talkgroup 19535 from the voice
LC while DSDcc reports 150607 from the embedded LC; the transmission
genuinely carries both). Don't treat the values as interchangeable
across backends.

#### TETRA (`protocol":"tetra"` / `"tetrakit"`) — real examples

TETRA runs a different chain and a different backend per hint (see the
[TETRA section](#tetra-protocoltetra--protocoltetrakit) above), so its events
look different from the FM/DSD ones: no `slot`, and the network identity rides
in `extra`. The frames below are the exact wire JSON the two backends emit.

**osmo (`protocol":"tetra"`, `tetra-rx` / TETMON).** The sync/broadcast frames
are from the real off-air UK capture (colour code 17, MCC `00ea`=234, MNC
`004e`=78); the call frame is the parser's pinned `DSETUPDEC` input — real
TETMON shape, placeholder SSIs — since that capture carried no live call:

```json
{"type":"event","kind":"sync","talkgroup":"","source_id":"","slot":"","color_code":"17","ran":"","nac":"","emergency":"","alias":"","crc_error":"0","message":"","extra":"mcc=00ea; mnc=004e; la=0; dlf=0; ulf=0; crypt=0; func=NETINFO1","raw":"TETMON_begin FUNC:NETINFO1 CCODE:17 MCC:00ea MNC:004e DLF:0 ULF:0 LA:0 CRYPT:0 RX:1 TETMON_end"}
{"type":"event","kind":"sync","talkgroup":"","source_id":"","slot":"","color_code":"","ran":"","nac":"","emergency":"","alias":"","crc_error":"0","message":"","extra":"la=6189; dlf=393087500; func=FREQINFO1","raw":"TETMON_begin FUNC:FREQINFO1 DLF:393087500 LA:6189 RX:1 TETMON_end"}
{"type":"event","kind":"call","talkgroup":"222","source_id":"1234567","slot":"","color_code":"","ran":"","nac":"","emergency":"","alias":"","crc_error":"0","message":"","extra":"idx=12; cid=5; nid=9; func=DSETUPDEC","raw":"TETMON_begin FUNC:DSETUPDEC IDX:12 SSI:1234567 SSI2:222 CID:5 NID:9 RX:1 TETMON_end"}
```

`NETINFO1` puts the colour code in `color_code` and the network id / crypt flag
in `extra`; `FREQINFO1` carries a neighbour carrier (`dlf` in Hz); `DSETUPDEC`
is a call with the calling `SSI` → `source_id` and the called `SSI2` →
`talkgroup`. (`AFCVAL` and other PHY diagnostics classify as `unknown` and are
suppressed by default.)

**tetra-kit (`protocol":"tetrakit"`, `decoder` JSON).** The traffic and MLE
broadcast frames are from the same real capture; the `D-SETUP` call frame is
the parser's pinned CMCE input:

```json
{"type":"event","kind":"voice","talkgroup":"","source_id":"0","slot":"","color_code":"","ran":"","nac":"","emergency":"","alias":"","crc_error":"0","message":"","extra":"service=UPLANE; pdu=TCH_S; usage_marker=0; dl_usage_marker=62; encr=0","raw":"{\"service\":\"UPLANE\",\"pdu\":\"TCH_S\",\"tn\":2,\"fn\":3,\"mn\":33,\"ssi\":0,\"usage marker\":0,\"address_type\":0,\"downlink usage marker\":62,\"encryption mode\":0,\"uzsize\":1380,\"zsize\":157,\"frame\":\"eJzlUlsOwCAI...\"}"}
{"type":"event","kind":"call","talkgroup":"","source_id":"1234567","slot":"","color_code":"","ran":"","nac":"","emergency":"","alias":"","crc_error":"0","message":"","extra":"service=CMCE; pdu=D-SETUP; usage_marker=3","raw":"{\"service\":\"CMCE\",\"pdu\":\"D-SETUP\",\"ssi\":1234567,\"usage marker\":3}"}
{"type":"event","kind":"unknown","talkgroup":"","source_id":"16777215","slot":"","color_code":"","ran":"","nac":"","emergency":"","alias":"","crc_error":"0","message":"","extra":"service=MLE; pdu=D-NWRK-BROADCAST; usage_marker=0; encr=0","raw":"{\"time\":\"2026-08-31T13:36:05Z\",\"service\":\"MLE\",\"pdu\":\"D-NWRK-BROADCAST\",\"tn\":4,\"fn\":3,\"mn\":33,\"ssi\":16777215,\"usage marker\":0,\"encryption mode\":0,\"address_type\":1,\"actual ssi\":16777215,\"number of neighbour cells\":3,\"cell 0\":[{\"Cell identifier CA\":11},{\"LA\":6189}]}"}
```

tetra-kit's focus is the traffic channel: `UPLANE`/`TCH_S` is `voice`, with the
speech itself carried (base64+zlib) in the report's `frame` field for the codec
path (see [TETRA_VOICE.md](TETRA_VOICE.md)), not in the event. On a traffic
burst the `ssi` is often `0` or the all-ones broadcast id (`16777215`) — the
real caller SSI arrives on the `D-SETUP` and is associated by `usage_marker`, so
`talkgroup`/`color_code` stay `""` here (unlike the osmo backend). PDUs that are
neither call nor traffic (e.g. the `MLE` `D-NWRK-BROADCAST` system info) pass
through as `unknown`, suppressed by default but carrying their fields.

#### Paging (`protocol":"pager-auto"` / `"flex"`) — real examples

Captured from the server decoding multimon-ng's bundled off-air recordings
(a POCSAG 1200 sample and the Dutch P2000 FLEX network), FM-modulated into IQ:

```json
{"type":"event","kind":"page","talkgroup":"273040","source_id":"","slot":"","color_code":"","ran":"","nac":"","emergency":"","alias":"","crc_error":"0","message":"+++TIME=0008300324+++TIME=0008300324","extra":"protocol=pocsag; baud=1200; message_type=alpha; function=3","raw":"{\"demod_name\":\"POCSAG1200\",\"address\":273040,\"function\":3,\"alpha\":\"+++TIME=0008300324+++TIME=0008300324<NUL>\"}"}
{"type":"event","kind":"page","talkgroup":"1523020","source_id":"","slot":"","color_code":"","ran":"","nac":"","emergency":"","alias":"","crc_error":"0","message":"Passage Ambulance Wilhelminabrug Leiden","extra":"protocol=flex; baud=1600; message_type=alpha; flex_type=alphanumeric; levels=2; phase=A; cycle=14; frame=118; addr_type=S; group=0; fragment=complete","raw":"{\"timestamp\":\"2026-09-29 12:54:04\",\"baud\":1600,\"level\":2,\"phase\":\"A\",\"cycle\":14,\"frame\":118,\"capcode\":1523020,\"addr_type\":\"S\",\"is_group\":false,\"msg_type\":\"alphanumeric\",\"type_tag\":\"ALN\",\"fragment\":\"complete\",\"msg_number\":0,\"retrieval\":0,\"maildrop\":0,\"k_ok\":true,\"sig_ok\":true,\"message\":\"Passage Ambulance Wilhelminabrug Leiden\"}"}
{"type":"event","kind":"sync","talkgroup":"","source_id":"","slot":"","color_code":"","ran":"","nac":"","emergency":"","alias":"","crc_error":"0","message":"","extra":"protocol=flex; baud=1600; info_type=biw_date; day=4; month=9; year=2024","raw":"{\"timestamp\":\"2026-09-29 12:54:04\",\"baud\":1600,\"level\":2,\"phase\":\"A\",\"cycle\":14,\"frame\":115,\"msg_type\":\"biw_date\",\"biw_position\":1,\"type_tag\":\"BIW_DATE\",\"year\":2024,\"month\":9,\"day\":4}"}
```

### Binary frames — decoded voice audio

1-byte tag followed by payload. Only one tag exists:

| byte 0 | payload |
|---|---|
| `0x01` | Decoded voice PCM, little-endian signed 16-bit. |

Both backend builds emit **8000 Hz mono** PCM, so a client handles audio
the same way regardless of which one answered:

- **`dsd-server` (dsd-fme subprocess)**: dsd-fme's DMR mode (`-fs`) emits
  8000 Hz **stereo** (TDMA slot 1 on the left channel, slot 2 on the
  right); the server collapses it to mono before it reaches the wire,
  auto-following the active slot — it streams the channel for whichever
  slot the decoder currently reports voice/call activity on, and falls
  back to an (L+R) downmix while no slot is known yet (nothing decoded, or
  concurrent voice on both slots). One WebSocket frame per UDP packet
  (typically tag + 320 bytes = 20 ms after the downmix). The other
  stereo modes (auto `-fa`, P25 Phase 2, X2-TDMA) are handled the same
  way; the single-channel modes (P25 Phase 1, NXDN, dPMR, D-STAR, YSF,
  EDACS/ProVoice) already emit mono and are relayed unchanged.
- **`dsd-server-dsdcc`**: **8000 Hz mono** natively, one frame per decoded
  voice burst (typically tag + 320 bytes = 20 ms). It likewise follows one
  slot's voice, so two concurrent DMR calls don't interleave into one
  garbled stream; single-slot and non-TDMA audio is never withheld.

In both cases, when both TDMA slots carry voice at once only the followed
slot is streamed — correlate with `event` frames (their `slot` field) if
you need to know which slot the audio belongs to.

There is no end-of-audio marker; audio frames simply stop when the
transmission ends or the pipeline is stopped.

---

## Lifecycle summary

```
connect                     ◀── {"type":"capabilities",...}  (greeting, once)
        ──▶ (optional binary IQ: silently ignored)
        ──▶ {"type":"start",...} ──▶ {"type":"started",...}
        ──▶ binary IQ frames    ──▶ {"type":"event",...}   (interleaved,
                                ──▶ 0x01 + PCM frames       any order)
        ──▶ {"type":"stop"}         (audio/events stop; socket stays open)
        ──▶ {"type":"start",...}    (new pipeline on same socket is fine)
```

- `capabilities` is the first frame sent, before the client transmits
  anything; a client reading the first frame for `started` should skip it.
- `started` always precedes that pipeline's events/audio; after that,
  event and audio frames interleave in whatever order decoding produces
  them — assume no ordering between the two streams.
- Any malformed input produces a single `error` frame and never closes
  the connection.
- Disconnecting (cleanly or abruptly) tears down the session's pipeline
  server-side; there is no shutdown handshake in the protocol itself.
- The server does **not** drop an idle connection for inactivity: a client
  may open the socket and send nothing (e.g. a health check that just
  verifies the server is up), or run a decode session that goes quiet for
  minutes on an idle channel, and the connection stays open. TCP keep-alive
  is enabled so a genuinely dead peer is still reaped by the OS. (Only the
  initial HTTP request, before the WebSocket upgrade, has a 30 s read
  deadline.)

## HTTP status endpoint (out of band)

The listening port double-serves a read-only HTTP status surface for
**non-WebSocket** GET requests, so it is not part of the WebSocket
protocol above but shares the port. A request with the WebSocket
`Upgrade` header becomes a session as normal; a plain GET is answered
over HTTP and the connection closed:

| Method + path | Response |
|---|---|
| `GET /`, `GET /status` | `text/html` dashboard; live-polls `/status.json` (~1 s) and patches the DOM in place, with a 5 s `<noscript>` meta-refresh fallback |
| `GET /status.json` | `application/json` (see below) |
| `GET /log.json` | `application/json` — the recent outbound JSON frames (see below) |
| `GET /log/clear` | empties the log ring buffer (the page's Clear button); returns `{"log":[]}` |
| `GET /iq_log/on` | turns the global IQ-capture switch on (every active/new session captures); returns `{"iq_log_enabled":true}` |
| `GET /iq_log/off` | turns it off (finalizes every session's capture); returns `{"iq_log_enabled":false}` |
| `GET /net` | `text/html` network explorer (calls / talkgroups / radios / networks and their associations; polls `/net.json`); sent with `Cross-Origin-Opener-Policy: same-origin` and `Cross-Origin-Embedder-Policy: credentialless` so it can be cross-origin isolated (multi-threaded speech-to-text) |
| `GET /net/manual.pdf` | `application/pdf` (inline) — the explorer's user manual, built into the server from `docs/NET_EXPLORER.pdf`; `404` if the build had none |
| `GET /net.json` | `application/json` association model (see below) |
| `GET /net/clear` | forgets everything the explorer learned, and its imports (`?audio=1` also deletes the call audio files); returns `{"ok":true,"audio_files":N,"audio_bytes":B}` |
| `GET /net/networks/merge?fam=&from=&to=` | network merge: show network `from` (and any merged into it) as part of `to`, for every viewer; returns `{"ok":true\|false,"merges":{…}}` (`ok` false = nothing changed) |
| `GET /net/networks/unmerge?fam=&key=` | undo: a merged network becomes its own again, or a network others were merged into lets them all go; same response |
| `GET /net/export.json` | `application/json` attachment `net_export_<UTC>.json` — the explorer export (below) |
| `GET /net/export.graphml` | `application/graphml+xml` attachment — the association graph for graph tools |
| `GET /net/log/on` | starts recording every input of the explorer's model to `net_<UTC>.jsonl.gz` (`?clear=1` clears the model first so the recording replays exactly); returns the recording status |
| `GET /net/log/off` | stops recording (the file ends with a snapshot of the model); returns the recording status |
| `GET /net/log/download` | `application/gzip` — the current or most recent recording; `404` if there is none |
| `GET /net/audio/on` | starts recording each call's decoded voice (off by default; see `/net.json` `audio`); returns the audio status. `on` / `off` is remembered for the next start (`DSD_NET_SETTINGS_FILE`, default `net_settings.json` in the recordings folder) unless `DSD_NET_AUDIO` is set |
| `GET /net/audio/off` | stops recording call audio (finished files stay playable); returns the audio status |
| `GET /net/audio/<file>.wav` | `audio/wav` — a call's audio (the `audio` file a call lists); supports `Range` (`206`); `404` for any name the server didn't create |
| `GET /net/asr/config.json` | `application/json` — the explorer's speech-to-text assets (`DSD_NET_ASR_DIR`): `{"local","lib","models":[…],"model","language","dir"}` — `local` when the library and at least one model are present |
| `GET /net/asr/<path>` | a file of that folder (`transformers.min.js`, `ort/…`, `models/<org>/<name>/…`), streamed; `ETag` / `If-None-Match` (`304`); `404` for anything else (no `..` or hidden names) |
| `GET /net/asr_worker.js` | `text/javascript` — the explorer's speech-to-text Web Worker |
| `POST /net/import?name=<label>` | body: an explorer export (JSON, or gzip) — added to the live view as an import layer; returns `{"name","status","message","id"}` (below); `400` if it isn't an export |
| `GET /net/imports/remove?id=N` | removes one import (`404` if no such id); `GET /net/imports/clear` removes all; return `{"ok":…}` |
| `POST /net/merge` | body `{"files":[{"name":…,"text":…}]}` (each export's text verbatim) — merges them without touching the live data; returns `{"report":[{"name","status","message"}],"export":{…merged export…}}` |
| any other path | `404` |
| other methods | `405` (request bodies up to 128 MB) |

`GET /status.json` returns a single object:

```json
{
  "total_sessions": 42,
  "current_sessions": 2,
  "active_pipelines": 1,
  "uptime_seconds": 3600,
  "iq_logging": false,
  "started": "2026-09-22 17:12:57Z",
  "by_protocol": { "dmr": 1 },
  "sessions": [
    { "id": 41, "remote": "10.0.0.5:52233", "protocol": "dmr", "chain": "fm",
      "protocols_used": ["nxdn48", "dmr"], "protocols_requested": ["nxdn48", "dmr"],
      "active": true, "connected": "2026-09-22 18:10:41Z", "duration_seconds": 71 }
  ],
  "history": [
    { "id": 40, "remote": "10.0.0.5:52190", "protocol": "nxdn48", "chain": "fm",
      "connected": "2026-09-22 18:05:02Z", "ended": "2026-09-22 18:09:55Z",
      "protocols_used": ["nxdn48"], "protocols_requested": ["tetra", "nxdn48"],
      "duration_seconds": 293 }
  ],
  "protocols": [
    { "protocol": "dmr", "chain": "fm", "requests": 31, "starts": 31, "failed": 0,
      "sessions": 28, "active": 1, "decode_seconds": 5120,
      "first_requested": "2026-09-22 17:13:40Z", "last_requested": "2026-09-22 18:10:41Z" },
    { "protocol": "tetra", "chain": "tetra", "requests": 1, "starts": 0, "failed": 1,
      "sessions": 0, "active": 0, "decode_seconds": 0,
      "first_requested": "2026-09-22 18:05:02Z", "last_requested": "2026-09-22 18:05:02Z" }
  ]
}
```

- `total_sessions` is cumulative since start; `current_sessions` is live
  WebSocket connections right now; `active_pipelines` is how many are
  decoding. HTTP status requests are **not** counted as sessions.
- `iq_logging` is the global IQ-capture switch state (the status page's
  "Log IQ" checkbox / `GET /iq_log/on`|`off`). When `true`, every active
  and newly started session captures its raw IQ to a BLUE file.
- A session's `protocol`/`chain` are `-`/`""` until it sends `start`;
  `chain` is `fm` (FM + DSD), `tetra` or `pager`. `active` flips to `false`
  on `stop` while the row keeps its last protocol label.
- `protocols_requested` lists every protocol the session asked for (in
  first-request order); `protocols_used` the ones whose pipeline actually
  started. A client can switch protocols with a new `start` on the same
  connection, so both can hold several entries.
- `history` is the most recent finished sessions (newest first, bounded to
  the server's history limit — 50 by default), each with the `ended`
  wall-clock time and total connected `duration_seconds`. It is in-memory
  only and resets on restart. The HTML page shows it under a **History**
  tab next to **Sessions**.
- `protocols` is a **run-wide** record of every protocol requested since
  the server started (most recently requested first), not only the live
  ones:
  - `requests` counts `start` messages asking for it, and `starts` counts
    those whose pipeline came up; `failed` is the difference (e.g. a
    missing decoder or an invalid key).
  - `sessions` counts distinct sessions that ran it, and `active` how many
    are decoding it right now.
  - `decode_seconds` is total pipeline run time across all sessions,
    including still-running ones.

  It is unaffected by the history limit (the set of protocol labels is
  fixed, so it can't grow without bound) and resets on restart. The HTML
  page shows it under a **Protocols** tab.
- Unlike the WebSocket frames, this JSON is **nested** (a `sessions`
  array, a `by_protocol` object) — it is a separate diagnostic surface,
  not a wire event.

`GET /net.json` returns the network explorer's association model, keyed by
protocol family (`dmr`, `p25`, `nxdn`, `tetra`, `dpmr`, `dstar`, `ysf`,
`edacs`, `x2tdma`; paging is not modelled). Times are epoch milliseconds;
`now` is the server's clock so a client can compute ages without skew:

```json
{"version": 812, "now": 1790000000000,
 "instance": "3f9a0c1d22b4e871", "name": "rx-north", "since": 1789999000000,
 "rec": {"on": false, "truncated": false, "file": "net_20261001_214359.jsonl.gz",
         "path": "/captures/net_20261001_214359.jsonl.gz", "bytes": 86317, "file_bytes": 9466},
 "audio": {"on": true, "dir": "/captures/net_audio", "bytes": 52428800, "cap_bytes": 1073741824, "files": 412, "recording": 1},
 "max_calls": 5000, "rates": {"p25": {"per_s_1m": 13.27, "per_s_10m": 12.81, "total": 48210}},
 "imports": [{"id": 1, "label": "south.json", "exported": 1789998000000,
              "sources": [{"instance": "a71e…", "name": "rx-south", "since": 1789990000000, "through": 1789998000000}],
              "networks": 2, "talkgroups": 14, "radios": 40, "calls": 120}],
 "families": {
  "p25": {
    "networks":   [{"key": "wacn:BEE00/sys:3A1", "label": "WACN BEE00 · SYS 3A1", "confidence": "strong",
                    "ids": {"nac": "293", "rfss": "4", "site_id": "12", "system_id": "3A1", "wacn": "BEE00"},
                    "sites": ["RFSS 4 · Site 12", "RFSS 4 · Site 13"], "freqs": [851012500, 852137500],
                    "sessions": 2, "calls": 22,
                    "first": 1789999970000, "last": 1789999990000}],
    "talkgroups": [{"id": "100", "networks": ["wacn:BEE00/sys:3A1"], "radios": {"10001": 2, "12001": 1},
                    "calls": 3, "emerg": 0, "enc": 0, "first": 1789999971000, "last": 1789999989000}],
    "radios":     [{"id": "10001", "aliases": [], "tgs": {"100": 2, "200": 1}, "peers": {},
                    "networks": ["wacn:BEE00/sys:3A1"], "calls": 3, "first": 1789999971000, "last": 1789999989000}],
    "calls":      [{"id": 41, "session": 3, "net": "wacn:BEE00/sys:3A1", "site": "RFSS 4 · Site 12",
                    "freq": 851012500, "slot": "",
                    "src": "10001", "tgt": "100", "alias": "", "text": "", "priv": false, "voice": true,
                    "data": false, "emerg": false, "enc": false, "open": false, "streams": 1,
                    "start": 1789999988000, "last": 1789999989000}]
  }}}
```

- `instance` identifies this server run (random per process), `name` is
  `DSD_SERVER_NAME` or the host name, `since` is when the live data's span
  began (server start or the last Clear). `imports` lists the exports imported
  into the view (see *Merging* below); `families` shows the live data and the
  imports merged.
- `merges` are the network merges in effect, per protocol: network key → the
  key it is shown under (`{"dmr": {"cc:1@436627500": "cc:1@436625000"}}`).
  They are rules for display -- `families` still lists the networks apart,
  and the explorer shows each group as one. Always flat (a target is never
  itself merged). Set with `/net/networks/merge` / `unmerge`; shared by every
  viewer, kept through Clear and restarts (`DSD_NET_MERGES_FILE`, default
  `net_merges.json` in the recordings folder), and an import adds its
  export's rules (this server's own win).
- `ui` is a fingerprint of the explorer page the server serves (also in
  `/status.json`, for the status page): a page whose own build differs reloads
  itself, so open pages pick up a server update.
- `dev` is `true` when the server runs with `DSD_NET_DEV=1`: the explorer then
  shows its developer tools (Record) to everyone; otherwise only to a browser
  that opened `/net?dev=1`. (The recording endpoints work either way.)
- `max_calls` is how many calls are kept per protocol (`DSD_NET_MAX_CALLS`,
  default 5000; when full, calls without audio roll off first). `rates` gives
  per protocol `per_s_1m` / `per_s_10m` (calls per second over the last 1 / 10
  minutes, or since start / Clear if shorter) and `total` (calls counted since
  then), from every call -- not only those still listed. Sent gzip-encoded
  when the request has `Accept-Encoding: gzip`.
- `audio` is the per-call audio status: `on`, the directory, bytes used of
  `cap_bytes`, the number of files, and how many calls are recording now.
  A call with audio carries `"audio": "<file>.wav"` (play it from
  `GET /net/audio/<file>.wav`) and `"audio_ms"` (its length so far); calls
  without audio have neither key. Audio is recorded per DMR slot (each of
  two concurrent calls gets only its own slot), starts with up to 1 s heard
  just before the call was decoded, keeps one recording when two receivers
  hear one call, and is never recorded for an encrypted call unless the
  session has the key. A recording starts only with audible audio (peak
  above about -54 dBFS) and is deleted at the call's end if under 0.2 s, so
  there are no empty files. Recording snapshots and imported exports never
  carry audio (an import's calls refer to files on another server).
- `rec` is the recording status: `on`, the current / most recent `file`
  (and full `path`), uncompressed `bytes` written, compressed `file_bytes` on
  disk, and `truncated` once the size cap was hit. The file format is
  documented in `src/assoc_log.hpp`; replay it with `net-replay`.
- `confidence` is `strong` (a system identity: P25 WACN+SysID, DMR network
  id, NXDN system code, TETRA MCC+MNC); `channel` (a short code -- DMR color
  code, NAC, RAN -- or nothing, on a known channel frequency: key suffixed
  `@<Hz>`, e.g. `cc:1@434425000`, or `ch@<Hz>` when unidentified; every
  stream on that channel with that code shares it); `weak` (a short code on
  an unknown frequency; scoped to one stream, key suffixed `@s<session>`) or
  `none` (`unknown:s<session>`: nothing decoded, frequency unknown).
- `freqs` on a network are the channels (Hz) it was heard on; `freq` on a
  call is its channel (0 = unknown). Channel frequencies are the `start`'s
  `center_freq + freq_offset`, snapped to `DSD_NET_FREQ_STEP_HZ` (default
  1250 Hz: the 6.25 kHz and 2.5 kHz channel plans are unchanged, and an
  offset up to ±625 Hz off still lands on its channel). A `set_freq_offset`
  moves the stream to the new channel (its identity starts over).
- `radios` on a talkgroup / `tgs` on a radio are call counts per association;
  `peers` are private (unit-to-unit) calls in either direction.
- A session appears only once it decodes real traffic, and when it ends any
  network it fed that never carried a call is removed (unless another running
  session is on it).
- `calls` holds the most recent calls (newest first, up to `max_calls`). `open` = still
  running (heard within the last 4 s); `streams` = how many receivers heard the
  same call (deduplicated on a shared network).
- Like `/status.json`, this JSON is nested and is a diagnostic surface, not
  part of the WebSocket protocol.

**Explorer export** (`GET /net/export.json`, the page's *Export -> Explorer
data*): the same `families` object as `/net.json` under a self-describing
header, so the explorer (and other tools) can open it later:

```json
{"format": "dsd-net-export", "format_version": 1, "source": "dsd-server",
 "instance": "3f9a0c1d22b4e871", "name": "rx-north",
 "exported": 1790000000000, "now": 1790000000000,
 "sources": [{"instance": "3f9a0c1d22b4e871", "name": "rx-north", "since": 1789999000000, "through": 1790000000000}],
 "merges": {"dmr": {"cc:1@436627500": "cc:1@436625000"}},
 "families": { ... as /net.json ... }}
```

`merges` (only when there are any) are the network merges -- as in
`/net.json` -- about networks the export holds (at least one end of each
rule; the other may be in another receiver's export). Readers apply them for
display; a reader that ignores them sees the networks apart.

`sources` is the export's provenance: for each server run whose data it holds
(this run's live data, plus anything it had imported), the span covered. A
merge of several files has `"instance": ""` and lists every source. Exports
from before provenance existed (no `sources`) are still read.

`format_version` increases only on incompatible changes; readers should
ignore keys they don't know. `GET /net/export.graphml` serves the same
associations as GraphML: node ids are `<protocol>:<r|t|n>:<id>` (radio,
talkgroup, network); node attributes `type`, `protocol`, `label`, `calls`,
`aliases`, `networks`, `radios`, `talkgroups`, `emergencies`, `encrypted`,
`confidence`, `identifiers`, `sites`, `first_seen`, `last_seen` (ISO 8601);
edge attributes `type` (`talkgroup` | `private` | `member`), `weight` and
`calls`.

**Merging** (the explorer's *Open...* with several files, *Import...*,
`POST /net/merge`, `POST /net/import`, and the `net-merge` tool all use the
same merge, `src/assoc_merge.hpp`):

- Per protocol, talkgroups and radios with the same id are joined, as are
  networks with the same *strong* key -- that is how receivers link up. A weak
  or unidentified network key is only meaningful within the server run that
  made it, so an imported one is qualified with the run
  (`cc:1@s3~3f9a0c1d`) and its label gets ` · <name>`. A `channel` key names
  a conventional channel and joins across runs and receivers too -- unless
  the server runs with `DSD_NET_CHANNEL_MERGE=receiver` (or `net-merge
  --per-receiver`), for receivers far enough apart to hear different systems
  on one frequency: then each receiver's is qualified with its name
  (`cc:1@434425000~south`), except the live server's own.
- Counts add up, sets are unioned, first / last span both. Calls are listed
  newest first (at most 1000 per protocol in a merge); a call two sources both
  heard -- same network (so a strong or channel key), source, target and
  kind, no more than 4 s apart -- is
  kept once with `streams` summed and counted once (only calls still in the
  lists can be matched this way).
- Nothing is counted twice: a file whose sources are all already included is
  `skipped`; one that contains an earlier import of the same run (a newer
  export) `replaced` it; one that partly overlaps (same run, overlapping time,
  neither containing the other) is `refused`; this server's own live data is
  `skipped` on import, and a file overlapping it is `refused` (Clear first).
  Data from different runs, or from before and after a Clear, merges freely.
  Other statuses: `imported` / `merged`, and `invalid` (not an export -- e.g.
  a recording -- with the reason).
- Network merges (`merges`) are combined too; an imported rule's keys are
  qualified like its networks', and a rule about another run's stream-scoped
  network is dropped. Two sources' records of one call on networks merged
  into one count as one call. GraphML output applies the merges (one network
  node per group).
- Imported calls get ids `<import id> × 10⁹ + n`, apart from live call ids.
  Imports last until removed or Clear; recordings never include them.

`GET /log.json` returns the recent JSON frames the server has sent clients
— for the status page's **Log** tab — as a bounded, in-memory ring (last
300, newest first, reset on restart):

```json
{ "log": [
  { "session": 41, "time": "2026-09-25 15:43:10Z",
    "text": "{\"type\":\"event\",\"kind\":\"call\",\"talkgroup\":\"19535\", ... }" }
] }
```

- `text` is each outbound frame verbatim, embedded as a JSON string.
- Binary voice PCM is never logged, and the high-rate `kind:"voice"`
  events are skipped so they can't flood the ring; every other frame
  (`capabilities`, `started`, `error`, and the non-voice `event`s) is kept.
  `status.json` reports the current line count as `log_lines`.

## Testing a client against a fake server

A client project does **not** need the real server (Boost/DSDcc/dsd-fme/
real IQ) to unit-test its code against this protocol. `tools/fake_dsd_server.hpp`
is a single, dependency-free (C++17 + POSIX sockets + `std::thread`) header
that speaks the exact wire protocol above but does no DSP. Drop it into a
test target and:

- **script the server → client direction** — `send_started()` (with a
  chosen `udp_audio_port` to mimic either backend), `send_event(Event{}…)`,
  `send_audio(pcm)`, `send_error(msg)`, plus raw/close/drop escape hatches
  for negative tests; and
- **assert on the client → server direction** — every control frame the
  client sends is recorded (`control_messages()`, `last_start()`), and
  streamed IQ is counted (`iq_bytes_received()` / `iq_frames_received()`).

Like the real server, the fake greets each connection with a
`capabilities` frame (a fixed representative one) before any control frame;
set `Options::send_capabilities = false` to test a client against a server
that doesn't advertise capabilities.

Scripting can be hung off two hooks so the fake mirrors the real server's
timing — which emits nothing until IQ is flowing: `on_control` fires per
control frame, and `on_iq` fires per binary IQ frame the client streams
(0-based `frame_index`). The bundled example (and the standalone runner)
does the minimum on `start` — just the `started` reply — then acquires the
signal (a sync + call event) on the first IQ push and streams a voice frame
on each push after that.

It runs in-process on a background thread (`start()` returns the bound
port; use `url()`), so a C++ test can drive it and assert without any IPC.
`tools/fake_dsd_server.cpp` is a standalone runner (`--port`, `--udp-port`)
for non-C++ or subprocess-style suites. See `tests/test_fake_dsd_server.cpp`
for a worked example (it validates the fake against an independent
Boost.Beast client). This is a testing aid for *client* projects; it is not
part of the server build.

## Protobuf schema for the control/event JSON

`proto/dsd_server.proto` is a proto3 schema that mirrors every JSON text
frame described above. It lets a client decode and build these messages as
generated, typed structs — using protobuf's canonical JSON mapping
(`JsonStringToMessage`/`MessageToJsonString` in C++, `protojson` in Go,
`json_format.Parse`/`MessageToJson` in Python, …) — instead of hand-plucking
JSON fields. It is a **client-side convenience artifact only**: the server
hand-writes and hand-parses its JSON and has no protobuf runtime dependency,
so `dsd_server.proto` is not built or shipped by the server. Compile it with
your own toolchain for your client's language.

Three things about the mapping are worth knowing:

- **Field names are snake_case.** Each multi-word field sets `json_name` to
  its wire key (`sample_rate`, `source_id`, `color_code`, `key_type`, …), so
  the default JSON printer emits the wire names — you do **not** need a
  "preserve proto field names" option, and the parser accepts them regardless.

- **Dispatch on `type` yourself.** Protobuf JSON can't pick a message from a
  discriminator field, so on receive parse each frame into `Envelope` first,
  switch on `type`, then parse the frame into the matching message
  (`Capabilities` / `Started` / `ErrorMessage` / `Event`). Parse a frame
  only into its own message — an `Event` frame parsed as `Started` would hit
  unknown fields unless your parser ignores them.

- **Numbers and default omission.** The numeric control fields are `double`,
  so a proto printer emits `2400000.0`; the server's parser reads that fine.
  proto3 JSON omits fields at their default value (`""`, `0`, `false`) on
  output — harmless for the server (it fills its own defaults), and harmless
  when parsing the server's always-every-key frames. If you re-serialize and
  need every key present, enable your library's "always print fields" option.

The schema is validated against the server's real emitted JSON: a
`StartRequest` built via protobuf and printed with the default JSON printer
is accepted verbatim by the server's own flat-JSON parser, and each real
`started` / `error` / `event` frame parses into its message with matching
field values.
