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
is unknown is an empty string `""`, never omitted and never `null`.

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
| `start` | `sample_rate` (default 2000000), `channel_bandwidth` (12500), `freq_offset` (0), `gain` (26000), `afc` (false), `protocol` (""), `key_type` (""), `key` ("") | Builds the demod + DSD pipeline. If a pipeline is already running it is stopped and rebuilt (clean restart). Replies with `started` on success, `error` on failure. |
| `set_gain` | `gain` (26000) | Live-adjusts discriminator gain. No reply. Ignored (silently) if no pipeline is running. |
| `set_freq_offset` | `hz` (0) | Live-adjusts the NCO shift. No reply. Ignored if no pipeline is running. Also resets any accumulated AFC correction (an explicit retune is a statement of new truth). |
| `stop` | — | Tears down the pipeline (kills the dsd-fme child / destroys the decoder). No reply. The WebSocket stays open; a new `start` is accepted afterwards. |

`freq_offset` / `hz` sign convention: **positive means the channel of
interest sits above 0 Hz in your IQ**, and the server mixes it down to
baseband. (Before AFC was added, the two demod implementations disagreed
on this sign — the hand-rolled demod had it inverted relative to the
liquid variant; it is now unified as stated and pinned by
`tests/test_afc.cpp`.)

`protocol`: an **advisory hint** telling the server which digital voice
protocol the client believes the signal is, so the decoder is told which
mode to run instead of relying on auto-detection. It is forgiving —
case-insensitive, and spaces/underscores/hyphens are ignored:

| value | selects | dsd-fme | DSDcc |
|---|---|---|---|
| `""` / absent | server default (**DMR**) — no change for existing clients | `-fs` | DMR |
| `dmr` | DMR | `-fs` | DMR |
| `nxdn48` (or `nxdn`, `idas`) | NXDN48 / IDAS (6.25 kHz) | `-fi` | NXDN48 |
| `nxdn96` | NXDN96 (12.5 kHz) | `-fn` | NXDN96 |
| `p25` (or `p25p1`) | P25 Phase 1 | `-f1` | P25p1 (mode only, no fields) |
| `p25p2` | P25 Phase 2 (6000 sps TDMA) | `-f2` | P25p1 (mode only, no fields) |
| `dpmr` | dPMR (6.25 kHz FDMA) | `-fm` | dPMR |
| `dstar` (or `d-star`) | D-STAR | `-fd` | D-STAR |
| `ysf` (or `fusion`, `c4fm`) | Yaesu System Fusion | `-fy` | YSF |
| `auto` / `unknown` / `not sure` / anything else | auto-detect | `-fa` | auto |

The hint only steers mode selection; it does not change the wire format
or the `event` shape. Note the two backends differ in NXDN capability:
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
(The FM/DSD *backend* choice — dsd-fme vs DSDcc vs liquid — is still build
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
  `afc`, `key_type`, `key` don't apply to the TETRA chain and are
  accepted-and-ignored (TETRA TEA encryption is not handled). `set_gain` /
  `set_freq_offset` are likewise accepted no-ops. (`protocol` itself is what
  selected this chain.)
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

---

## Server → Client

Four frame shapes total: three JSON text frames (`started`, `error`,
`event`) and one tagged binary frame (decoded audio). Nothing else is
ever sent.

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

### `error` — something was rejected

The connection **stays open** after every error; only the offending
message is affected. Exactly four message texts exist:

```json
{"type":"error","message":"unknown message type: <type>"}
{"type":"error","message":"bad control message: <parser detail>"}
{"type":"error","message":"binary frame length not a multiple of 8 bytes"}
{"type":"error","message":"failed to start DSD backend"}
```

| message | trigger |
|---|---|
| `unknown message type: ...` | Text frame parsed as JSON but its `type` isn't one of the four control types. The unrecognized type is echoed after the colon. |
| `bad control message: ...` | Text frame that isn't valid flat JSON (or a field with an impossible value that throws in parsing). The detail after the colon is the parser's exception text — useful for debugging, not stable enough to match on programmatically. |
| `binary frame length not a multiple of 8 bytes` | Malformed binary IQ frame received after `start`. The frame is dropped. |
| `failed to start DSD backend` | `start` couldn't bring up the decoder — for the subprocess backend, the fork/exec of dsd-fme failed (usually: no `dsd-fme` on the server's PATH); for the DSDcc backend, an unsupported config (e.g. a non-48 kHz internal rate). No pipeline is running after this; a corrected `start` may be retried. |

Match on the prefix up to the first `:` if you need to branch on error
kind; treat the remainder as free text.

### `event` — decoder activity

One frame per line the DSD backend reports (subprocess backend: one per
line dsd-fme writes to its log, post-cleanup; DSDcc backend: one per
detected state change). All twelve data fields are always present; empty
string means "not present in this event".

```json
{"type":"event","kind":"call","talkgroup":"19535","source_id":"2222223","slot":"2","color_code":"","ran":"","nac":"","emergency":"","alias":"","crc_error":"","extra":"","raw":" SLOT 2 TGT=19535 SRC=2222223 Group Call  "}
```

| field | type | meaning |
|---|---|---|
| `type` | string | `"event"` |
| `kind` | string | Best-effort classification: `"voice"`, `"sync"`, `"call"`, `"burst"` (DSDcc backend only), or `"unknown"`. See the per-backend notes below for exactly when each occurs. **On the subprocess backend, `unknown` events are suppressed by default** — dsd-fme prints a large startup banner / version / device-config block that all classifies as `unknown` noise, so it isn't forwarded (server-side `DsdProcessConfig::forward_unknown`; set it true to forward unrecognized lines for classifier debugging). Recognized events (`voice`/`sync`/`call`/`burst`) are always forwarded. |
| `talkgroup` | string | Decimal talkgroup / group-call target ID, or `""`. Kept as a string because IDs can exceed what a client might assume about integer width, and `""` is the natural "absent". |
| `source_id` | string | Decimal source radio ID, or `""`. |
| `slot` | string | TDMA slot, `"1"` or `"2"`, or `""` when the event isn't slot-specific. |
| `color_code` | string | DMR color code as bare decimal (`"4"`, not `"04"` — both backends normalize away leading zeros), or `""` when the event doesn't carry one. Which event kinds carry it differs by backend: dsd-fme prints it on its per-burst sync lines, DSDcc's slot status text carries it on `voice`/`call` events. |
| `ran` | string | NXDN Radio Access Number as bare decimal (`"2"`, not `"02"`), or `""`. The NXDN analog of `color_code` — a repeater-access/filter code — surfaced by the dsd-fme backend on NXDN sync lines that carry `RAN NN`. DMR events leave it `""`, NXDN events leave `color_code` `""`. |
| `nac` | string | P25 Network Access Code as uppercase hex without `0x` (`"293"`), or `""`. The P25 analog of `color_code`/`ran`. dsd-fme backend only. |
| `emergency` | string | `"1"` when the line flags the call as an emergency (high-priority traffic), else `""`. dsd-fme backend only. |
| `alias` | string | DMR talker-alias text (the operator's over-the-air alias) when dsd-fme has assembled and printed it, else `""`. Free text. dsd-fme backend only. |
| `crc_error` | string | `"1"` when the decoder itself marked this line/burst as failing an FEC/CRC check, else `""`. Subprocess backend: set when the cleaned dsd-fme line carries a `CRC ERR`, `FEC ERR`, or `EMB ERR` marker. DSDcc backend: set on `burst` events whose slot-type PDU failed its Golay(20,8) FEC (`burst=UNK`). Treat the flagged event's other fields (especially `color_code`) as unreliable; note the reverse does not hold — a marginal burst can decode "cleanly" to a wrong value without being flagged. |
| `extra` | string | Backend-specific detail that doesn't fit the fields above, or `""`. **Format: zero or more `key=value` tokens joined by `"; "`.** DSDcc backend tokens: `unit_target=<id>` (unit-to-unit call target — not a talkgroup, so kept out of `talkgroup`), `burst=<type>` on `burst` events (three-letter slot burst type: `IDL` idle, `CSB` CSBK control, `VLC`/`TLC` voice/terminator link control, `VOX` voice, `UNK` unknown, …), `sync_type=<flavor>` on sync-acquisition events (see below). dsd-fme backend NXDN trunking tokens: `site_code=<n>`, `system_code=<n>`, `location_id=<hex>`, `category=<name>` (e.g. `Global`). Because the same label can mean different things per line (a `Site Code` is the home site on a Site ID line but an adjacent site on an Adjacent Information line), read the accompanying `raw` for context. |
| `raw` | string | The underlying decoder output this event was parsed from, so nothing is lost to the classification: the cleaned log line (subprocess backend) or a synthesized description (DSDcc backend). Free text; formats below are examples from real decodes, and they **vary across dsd-fme versions/forks** — parse the structured fields, fall back to `raw` only for display/debugging. |

#### Which fields each protocol populates

Every field is **always present** in every `event` frame regardless of
protocol; this table says which ones ever carry a non-`""` value for a
given protocol (driven by the `protocol` start hint). A blank cell means
the field stays `""` for that protocol — not that it is omitted.

| field | DMR | NXDN | P25 | dPMR | D-STAR | YSF | notes |
|---|:---:|:---:|:---:|:---:|:---:|:---:|---|
| `type` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | always `"event"` |
| `kind` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | `burst` is DSDcc/DMR-only; NXDN/P25 control messages are `unknown` |
| `talkgroup` | ✓ | ✓ | ✓ | ✓ | ‡ | ‡ | DMR `TGT=`; NXDN `Dst/TG=`; P25 `TGT:`; dPMR `TG=` (zero-padding stripped). ‡ D-STAR/YSF: a **callsign** (the destination — e.g. `CQCQCQ`), not a numeric id |
| `source_id` | ✓ | ✓ | ✓ | ✓ | ‡ | ‡ | `Src=` / `SRC:` (zero-padding stripped). ‡ D-STAR/YSF: the transmitting station's **callsign** |
| `slot` | ✓ | — | — | — | — | — | DMR TDMA slot `1`/`2` |
| `color_code` | ✓ | — | — | ✓* | — | — | DMR color code / dPMR channel code; *dPMR: dsd-fme backend only |
| `ran` | — | ✓ | — | — | — | — | NXDN Radio Access Number |
| `nac` | — | — | ✓* | — | — | — | P25 Network Access Code (hex); *dsd-fme backend only |
| `emergency` | ✓* | ✓* | ✓* | ✓* | — | — | emergency flag; *dsd-fme backend only |
| `alias` | ✓* | — | — | — | — | — | DMR talker alias; *dsd-fme backend only |
| `crc_error` | ✓ | ✓ | ✓ | ✓ | ✓* | ✓* | FEC/CRC-failure flag (dsd-fme, and DSDcc for DMR/NXDN); *D-STAR/YSF: dsd-fme only |
| `extra` | ✓ | ✓ | ✓ | — | ✓ | ✓ | protocol/backend-specific `key=value` tokens (see below) |
| `raw` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | always the source line/description |

(`color_code`/`ran`/`nac` are the per-protocol access codes — DMR & dPMR,
NXDN, P25 respectively — so at most one is populated for a given signal.
D-STAR and YSF are amateur protocols that identify stations by **callsign**,
so none of the numeric access codes apply; `source_id`/`talkgroup` carry
callsign text instead of numbers, and the repeater/routing detail rides in
`extra`.)

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

Both backends emit the NXDN fields (`ran`, `source_id`, `talkgroup`, and
the `site_code`/`system_code`/`location_id` tokens) with the same shape,
so a client sees a consistent structure regardless of which backend
decoded. The DSDcc backend derives `system_code`/`site_code` from the
high/low 12 bits of its decoded location ID (the same split dsd-fme
prints); it does not currently surface `category`.

The DMR trunking / LC fields (`emergency`, `alias`, and the
`network_type` / `network_id` / `site_id` / `rest_channel` / `lcn`
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
{"type":"event","kind":"sync","talkgroup":"","source_id":"","slot":"2","color_code":"4","ran":"","nac":"","emergency":"","alias":"","crc_error":"","extra":"","raw":"20:37:20 Sync: +DMR   slot1  [SLOT2] | Color Code=04 | VC6 "}
{"type":"event","kind":"call","talkgroup":"19535","source_id":"2222223","slot":"2","color_code":"","ran":"","nac":"","emergency":"","alias":"","crc_error":"","extra":"","raw":" SLOT 2 TGT=19535 SRC=2222223 Group Call  "}
{"type":"event","kind":"sync","talkgroup":"","source_id":"","slot":"1","color_code":"5","ran":"","nac":"","emergency":"","alias":"","crc_error":"1","extra":"","raw":"13:37:43 Sync: +DMR  [slot1]  slot2  | Color Code=05 | MBCC (FEC ERR)"}
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
{"type":"event","kind":"call","talkgroup":"","source_id":"2048","slot":"","color_code":"","ran":"","nac":"","emergency":"","alias":"","crc_error":"","extra":"network_type=con+; lcn=3","raw":" Connect Plus Group Voice Channel Grant; Target: 100; Source: 2048; LCN: 3; TS: 1;"}
{"type":"event","kind":"unknown","talkgroup":"","source_id":"","slot":"","color_code":"","ran":"","nac":"","emergency":"","alias":"","crc_error":"","extra":"network_type=cap+; rest_channel=5","raw":" Capacity Plus Channel Status - FL: 1 TS: 1 RS: 0 - Rest LSN: 5"}
{"type":"event","kind":"unknown","talkgroup":"","source_id":"","slot":"","color_code":"","ran":"","nac":"","emergency":"","alias":"","crc_error":"","extra":"network_id=9; site_id=1","raw":" C_ALOHA_SYS_PARMS: Tier III; Net ID: 9; Site ID: 1;"}
{"type":"event","kind":"call","talkgroup":"","source_id":"2048","slot":"","color_code":"","ran":"","nac":"","emergency":"","alias":"JOHN SMITH","crc_error":"","extra":"","raw":" TG: 100; SRC: 2048; Talker Alias: JOHN SMITH"}
{"type":"event","kind":"call","talkgroup":"100","source_id":"2048","slot":"1","color_code":"","ran":"","nac":"","emergency":"1","alias":"","crc_error":"","extra":"","raw":" SLOT 1 TGT=100 SRC=2048 Group Emergency"}
```

- `emergency:"1"` flags a line carrying an emergency marker (the value
  forms `Emergency: <timer>` / `Emergency = <n>` do **not** trip it).
- `alias` is the talker-alias text once dsd-fme assembles it.
- `network_type`/`network_id`/`site_id`/`rest_channel`/`lcn` ride in
  `extra`; DSDcc leaves all of these empty (it doesn't decode CSBK).

##### P25 (subprocess backend with `protocol:"p25"` / `"p25p2"`)

The first two frames are **real** — from a P25 Phase 1 control-channel
capture; the rest (a voice call with encryption) are source-format
shapes:

```json
{"type":"event","kind":"sync","talkgroup":"","source_id":"","slot":"","color_code":"","ran":"","nac":"717","emergency":"","alias":"","crc_error":"","extra":"rfss=1; site_id=97","raw":"17:30:46 Sync: +P25p1 NAC/CC: 717; RFSS: 001; Site: 097;  TSBK"}
{"type":"event","kind":"unknown","talkgroup":"","source_id":"","slot":"","color_code":"","ran":"","nac":"","emergency":"","alias":"","crc_error":"","extra":"site_id=91; rfss=1; system_id=715","raw":" LRA [00] CFVA [3] RFSS[001] SITE [091] SYSID [715]"}
{"type":"event","kind":"call","talkgroup":"100","source_id":"2048","slot":"","color_code":"","ran":"","nac":"293","emergency":"","alias":"","crc_error":"","extra":"","raw":"P25 TGT: 00000100; SRC: 00002048; NAC: 293;"}
{"type":"event","kind":"unknown","talkgroup":"","source_id":"","slot":"","color_code":"","ran":"","nac":"","emergency":"","alias":"","crc_error":"","extra":"alg_id=84; key_id=0001","raw":" HDU  ALG ID: 0x84 KEY ID: 0x0001 MI: 0x0123456789ABCDEF"}
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
{"type":"event","kind":"call","talkgroup":"10011","source_id":"243","slot":"","color_code":"31","ran":"","nac":"","emergency":"","alias":"","crc_error":"","extra":"","raw":" TG=0010011 Src=0000243 Channel Code=31"}
{"type":"event","kind":"unknown","talkgroup":"","source_id":"","slot":"","color_code":"","ran":"","nac":"","emergency":"","alias":"","crc_error":"1","extra":"","raw":" TG=(CRC ERR) Src=(CRC ERR) Channel Code =(CRC ERR)"}
{"type":"event","kind":"call","talkgroup":"14653","source_id":"302","slot":"","color_code":"","ran":"","nac":"","emergency":"","alias":"","crc_error":"","extra":"","raw":"(dsdcc dpmr) own 302 called 14653"}
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
{"type":"event","kind":"call","talkgroup":"CQCQCQ","source_id":"F1NSR ID51","slot":"","color_code":"","ran":"","nac":"","emergency":"","alias":"","crc_error":"","extra":"rpt1=F1ZIL B; rpt2=F1ZIL G","raw":"18:27:55 Sync: -DSTAR VOICE   RPT 2: F1ZIL  G RPT 1: F1ZIL  B DST: CQCQCQ   SRC: F1NSR   ID51 REPEATER"}
{"type":"event","kind":"call","talkgroup":"CQCQCQ","source_id":"F1NSR /ID51","slot":"","color_code":"","ran":"","nac":"","emergency":"","alias":"","crc_error":"","extra":"rpt1=F1ZIL B; rpt2=F1ZIL B; radio_text=YANNICK ST RAPHAEL","raw":"(dsdcc dstar) my F1NSR /ID51 ur CQCQCQ"}
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
{"type":"event","kind":"call","talkgroup":"","source_id":"F1SER","slot":"","color_code":"","ran":"","nac":"","emergency":"","alias":"","crc_error":"","extra":"call_mode=group_cq; data_type=vd2","raw":"18:28:18 Sync: +YSF  V/D2 Group/CQ -Simplex CC FN: 2/7 SRC: F1SER     "}
{"type":"event","kind":"call","talkgroup":"","source_id":"","slot":"","color_code":"","ran":"","nac":"","emergency":"","alias":"","crc_error":"","extra":"call_mode=group_cq; data_type=vd2; uplink=F5ZOO-R1","raw":"18:28:18 Sync: +YSF  V/D2 Group/CQ -Simplex CC FN: 3/7 U/L: F5ZOO-R1  "}
{"type":"event","kind":"call","talkgroup":"","source_id":"F1SER","slot":"","color_code":"","ran":"","nac":"","emergency":"","alias":"","crc_error":"","extra":"call_mode=group_cq; data_type=vd2; uplink=F5ZOO-R1; downlink=F5ZOO-R1","raw":"(dsdcc ysf) src F1SER dst "}
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
{"type":"event","kind":"voice","talkgroup":"","source_id":"","slot":"","color_code":"","ran":"2","nac":"","emergency":"","alias":"","crc_error":"","extra":"","raw":"Sync: NXDN48  RTCH Voice  RAN 02 PF X/4"}
{"type":"event","kind":"call","talkgroup":"2043","source_id":"958","slot":"","color_code":"","ran":"","nac":"","emergency":"","alias":"","crc_error":"","extra":"","raw":" Session Call - ... - Src=958 - Dst/TG=2043 - Prefix Ch: 3 "}
{"type":"event","kind":"unknown","talkgroup":"","source_id":"","slot":"","color_code":"","ran":"","nac":"","emergency":"","alias":"","crc_error":"","extra":"site_code=1","raw":"Site ID Message - Area: 0; Site Type: 8 Narrow; Site Code: 1 Open Access;  FACCH3"}
{"type":"event","kind":"unknown","talkgroup":"","source_id":"","slot":"","color_code":"","ran":"","nac":"","emergency":"","alias":"","crc_error":"","extra":"site_code=2; system_code=8; category=Global","raw":"Adjacent Information - Cat: Global - Sys Code: 8 - Site Code 2 "}
{"type":"event","kind":"unknown","talkgroup":"","source_id":"","slot":"","color_code":"","ran":"","nac":"","emergency":"","alias":"","crc_error":"","extra":"location_id=008002","raw":"Service Information - Location ID [008002] SVC [01A8] RST [000000] "}
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
{"type":"event","kind":"sync","talkgroup":"","source_id":"","slot":"","color_code":"","ran":"","nac":"","emergency":"","alias":"","crc_error":"","extra":"sync_type=dmr_bs_data","raw":"(dsdcc: sync acquired, dmr_bs_data)"}
{"type":"event","kind":"burst","talkgroup":"","source_id":"","slot":"1","color_code":"4","ran":"","nac":"","emergency":"","alias":"","crc_error":"","extra":"burst=IDL","raw":"(dsdcc slot1) .04 IDL                   "}
{"type":"event","kind":"voice","talkgroup":"150607","source_id":"2222223","slot":"2","color_code":"4","ran":"","nac":"","emergency":"","alias":"","crc_error":"","extra":"","raw":"(dsdcc slot2) *04 VOX 02222223>G00150607"}
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
{"type":"event","kind":"call","talkgroup":"200","source_id":"100","slot":"","color_code":"","ran":"9","nac":"","emergency":"","alias":"","crc_error":"","extra":"","raw":"(dsdcc nxdn) RAN 9 src 100 dst 200 group"}
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

### Binary frames — decoded voice audio

1-byte tag followed by payload. Only one tag exists:

| byte 0 | payload |
|---|---|
| `0x01` | Decoded voice PCM, little-endian signed 16-bit. |

The PCM format depends on the backend build:

- **`dsd-server` (dsd-fme subprocess)**: whatever dsd-fme's UDP output
  produces, relayed verbatim. For the default DMR mode (`-fs`) on
  lwvmobile/dsd-fme that is **8000 Hz stereo interleaved** — TDMA slot 1
  on the left channel, slot 2 on the right — arriving as one WebSocket
  frame per UDP packet (typically 641 bytes: tag + 640 bytes = 20 ms).
- **`dsd-server-dsdcc`**: **8000 Hz mono**, one frame per decoded voice
  burst (typically tag + 320 bytes = 20 ms). When both TDMA slots carry
  voice simultaneously, each slot's audio arrives as its own frames,
  interleaved in time — there is no channel tag distinguishing them
  (correlate with `event` frames if you need attribution).

There is no end-of-audio marker; audio frames simply stop when the
transmission ends or the pipeline is stopped.

---

## Lifecycle summary

```
connect ──▶ (optional binary IQ: silently ignored)
        ──▶ {"type":"start",...} ──▶ {"type":"started",...}
        ──▶ binary IQ frames    ──▶ {"type":"event",...}   (interleaved,
                                ──▶ 0x01 + PCM frames       any order)
        ──▶ {"type":"stop"}         (audio/events stop; socket stays open)
        ──▶ {"type":"start",...}    (new pipeline on same socket is fine)
```

- `started` always precedes that pipeline's events/audio; after that,
  event and audio frames interleave in whatever order decoding produces
  them — assume no ordering between the two streams.
- Any malformed input produces a single `error` frame and never closes
  the connection.
- Disconnecting (cleanly or abruptly) tears down the session's pipeline
  server-side; there is no shutdown handshake in the protocol itself.

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
  (`Started` / `ErrorMessage` / `Event`). Parse a frame only into its own
  message — an `Event` frame parsed as `Started` would hit unknown fields
  unless your parser ignores them.

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
