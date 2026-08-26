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

---

## Client → Server

### Text frames (JSON control messages)

Each is a flat object selected by `"type"`. Unknown numeric fields fall
back to the defaults shown (see `handle_text_message` in `session.cpp`).

| type | fields | effect |
|---|---|---|
| `start` | `sample_rate` (default 2000000), `channel_bandwidth` (12500), `freq_offset` (0), `gain` (26000), `afc` (false), `protocol` ("") | Builds the demod + DSD pipeline. If a pipeline is already running it is stopped and rebuilt (clean restart). Replies with `started` on success, `error` on failure. |
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
detected state change). All nine data fields are always present; empty
string means "not present in this event".

```json
{"type":"event","kind":"call","talkgroup":"19535","source_id":"2222223","slot":"2","color_code":"","ran":"","crc_error":"","extra":"","raw":" SLOT 2 TGT=19535 SRC=2222223 Group Call  "}
```

| field | type | meaning |
|---|---|---|
| `type` | string | `"event"` |
| `kind` | string | Best-effort classification: `"voice"`, `"sync"`, `"call"`, `"burst"` (DSDcc backend only), or `"unknown"`. See the per-backend notes below for exactly when each occurs. |
| `talkgroup` | string | Decimal talkgroup / group-call target ID, or `""`. Kept as a string because IDs can exceed what a client might assume about integer width, and `""` is the natural "absent". |
| `source_id` | string | Decimal source radio ID, or `""`. |
| `slot` | string | TDMA slot, `"1"` or `"2"`, or `""` when the event isn't slot-specific. |
| `color_code` | string | DMR color code as bare decimal (`"4"`, not `"04"` — both backends normalize away leading zeros), or `""` when the event doesn't carry one. Which event kinds carry it differs by backend: dsd-fme prints it on its per-burst sync lines, DSDcc's slot status text carries it on `voice`/`call` events. |
| `ran` | string | NXDN Radio Access Number as bare decimal (`"2"`, not `"02"`), or `""`. The NXDN analog of `color_code` — a repeater-access/filter code — surfaced by the dsd-fme backend on NXDN sync lines that carry `RAN NN`. DMR events leave it `""`, NXDN events leave `color_code` `""`. |
| `crc_error` | string | `"1"` when the decoder itself marked this line/burst as failing an FEC/CRC check, else `""`. Subprocess backend: set when the cleaned dsd-fme line carries a `CRC ERR`, `FEC ERR`, or `EMB ERR` marker. DSDcc backend: set on `burst` events whose slot-type PDU failed its Golay(20,8) FEC (`burst=UNK`). Treat the flagged event's other fields (especially `color_code`) as unreliable; note the reverse does not hold — a marginal burst can decode "cleanly" to a wrong value without being flagged. |
| `extra` | string | Backend-specific detail that doesn't fit the fields above, or `""`. **Format: zero or more `key=value` tokens joined by `"; "`.** DSDcc backend tokens: `unit_target=<id>` (unit-to-unit call target — not a talkgroup, so kept out of `talkgroup`), `burst=<type>` on `burst` events (three-letter slot burst type: `IDL` idle, `CSB` CSBK control, `VLC`/`TLC` voice/terminator link control, `VOX` voice, `UNK` unknown, …), `sync_type=<flavor>` on sync-acquisition events (see below). dsd-fme backend NXDN trunking tokens: `site_code=<n>`, `system_code=<n>`, `location_id=<hex>`, `category=<name>` (e.g. `Global`). Because the same label can mean different things per line (a `Site Code` is the home site on a Site ID line but an adjacent site on an Adjacent Information line), read the accompanying `raw` for context. |
| `raw` | string | The underlying decoder output this event was parsed from, so nothing is lost to the classification: the cleaned log line (subprocess backend) or a synthesized description (DSDcc backend). Free text; formats below are examples from real decodes, and they **vary across dsd-fme versions/forks** — parse the structured fields, fall back to `raw` only for display/debugging. |

#### Which fields each protocol populates

Every field is **always present** in every `event` frame regardless of
protocol; this table says which ones ever carry a non-`""` value for a
given protocol (driven by the `protocol` start hint). A blank cell means
the field stays `""` for that protocol — not that it is omitted.

| field | DMR | NXDN48 / NXDN96 | notes |
|---|:---:|:---:|---|
| `type` | ✓ | ✓ | always `"event"` |
| `kind` | ✓ | ✓ | `burst` is DSDcc/DMR-only; NXDN control messages are `unknown` |
| `talkgroup` | ✓ | ✓ | DMR `TGT=`; NXDN `Dst/TG=` / `TGT:` |
| `source_id` | ✓ | ✓ | `Src=` / `SRC=` |
| `slot` | ✓ | — | TDMA slot `1`/`2`; NXDN48 is single-channel, so blank |
| `color_code` | ✓ | — | DMR color code |
| `ran` | — | ✓ | NXDN Radio Access Number (the NXDN analog of `color_code`) |
| `crc_error` | ✓ | ✓ | FEC/CRC-failure flag; both protocols on the dsd-fme backend |
| `extra` | ✓ | ✓ | protocol/backend-specific `key=value` tokens (see below) |
| `raw` | ✓ | ✓ | always the source line/description |

`extra` token vocabulary by protocol/backend:

| token | protocol | backend | meaning |
|---|---|---|---|
| `unit_target=<id>` | DMR | DSDcc | private-call target (not a talkgroup) |
| `burst=<type>` | DMR | DSDcc | slot burst type (`IDL`/`CSB`/`VLC`/`TLC`/`VOX`/`UNK`) |
| `sync_type=<flavor>` | DMR | DSDcc | `dmr_bs_data` / `dmr_ms_voice` / … |
| `site_code=<n>` | NXDN | both | site code (home or adjacent — see `raw`) |
| `system_code=<n>` | NXDN | both | trunked system code |
| `location_id=<hex>` | NXDN | both | site location ID |
| `category=<name>` | NXDN | dsd-fme | system scope, e.g. `Global` |

Both backends emit the NXDN fields (`ran`, `source_id`, `talkgroup`, and
the `site_code`/`system_code`/`location_id` tokens) with the same shape,
so a client sees a consistent structure regardless of which backend
decoded. The DSDcc backend derives `system_code`/`site_code` from the
high/low 12 bits of its decoded location ID (the same split dsd-fme
prints); it does not currently surface `category`.

Protocol vs. backend: **DMR** decodes on both the dsd-fme and DSDcc
backends. **NXDN** parses on both, but is only *reliable* on the dsd-fme
backend — the DSDcc backend's NXDN symbol recovery drops sync on real
off-air signals (it decodes clean/synthetic input fine), so prefer
dsd-fme for real NXDN (see the `protocol` field note above). P25/other
protocols are not yet parsed into structured fields — with
`protocol:"auto"` an unrecognized protocol's output still arrives, but
only in `raw` under `kind:"unknown"`.

Encoding guarantee: strings are JSON-escaped per RFC 8259 including
control characters (`\u00XX`), and the subprocess backend strips ANSI
color sequences and carriage returns from dsd-fme's output before
parsing — so `raw` is clean printable text and every frame is valid
JSON even though dsd-fme colorizes its terminal log.

#### Subprocess backend (`dsd-server`, real dsd-fme) — real examples

Captured from lwvmobile/dsd-fme decoding a real DMR group call
(the test suite's `session_real_fme_test`):

```json
{"type":"event","kind":"sync","talkgroup":"","source_id":"","slot":"2","color_code":"4","ran":"","crc_error":"","extra":"","raw":"20:37:20 Sync: +DMR   slot1  [SLOT2] | Color Code=04 | VC6 "}
{"type":"event","kind":"call","talkgroup":"19535","source_id":"2222223","slot":"2","color_code":"","ran":"","crc_error":"","extra":"","raw":" SLOT 2 TGT=19535 SRC=2222223 Group Call  "}
{"type":"event","kind":"unknown","talkgroup":"","source_id":"","slot":"","color_code":"","ran":"","crc_error":"","extra":"","raw":"Decoding DMR BS/MS Simplex"}
{"type":"event","kind":"sync","talkgroup":"","source_id":"","slot":"1","color_code":"5","ran":"","crc_error":"1","extra":"","raw":"13:37:43 Sync: +DMR  [slot1]  slot2  | Color Code=05 | MBCC (FEC ERR)"}
```

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
- `kind:"unknown"` — everything else dsd-fme prints: startup banner
  lines, `Activity Update ...` summaries, FEC error notes, the exit
  summary. These are deliberate pass-throughs (the `raw` field is the
  point), not noise to be alarmed by.

##### NXDN / IDAS (subprocess backend with `protocol:"nxdn48"`)

Captured from dsd-fme decoding a real off-air NXDN48/IDAS trunked
control channel (the same signal used to verify the `protocol` hint):

```json
{"type":"event","kind":"voice","talkgroup":"","source_id":"","slot":"","color_code":"","ran":"2","crc_error":"","extra":"","raw":"Sync: NXDN48  RTCH Voice  RAN 02 PF X/4"}
{"type":"event","kind":"call","talkgroup":"2043","source_id":"958","slot":"","color_code":"","ran":"","crc_error":"","extra":"","raw":" Session Call - ... - Src=958 - Dst/TG=2043 - Prefix Ch: 3 "}
{"type":"event","kind":"unknown","talkgroup":"","source_id":"","slot":"","color_code":"","ran":"","crc_error":"","extra":"site_code=1","raw":"Site ID Message - Area: 0; Site Type: 8 Narrow; Site Code: 1 Open Access;  FACCH3"}
{"type":"event","kind":"unknown","talkgroup":"","source_id":"","slot":"","color_code":"","ran":"","crc_error":"","extra":"site_code=2; system_code=8; category=Global","raw":"Adjacent Information - Cat: Global - Sys Code: 8 - Site Code 2 "}
{"type":"event","kind":"unknown","talkgroup":"","source_id":"","slot":"","color_code":"","ran":"","crc_error":"","extra":"location_id=008002","raw":"Service Information - Location ID [008002] SVC [01A8] RST [000000] "}
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
{"type":"event","kind":"sync","talkgroup":"","source_id":"","slot":"","color_code":"","ran":"","crc_error":"","extra":"sync_type=dmr_bs_data","raw":"(dsdcc: sync acquired, dmr_bs_data)"}
{"type":"event","kind":"burst","talkgroup":"","source_id":"","slot":"1","color_code":"4","ran":"","crc_error":"","extra":"burst=IDL","raw":"(dsdcc slot1) .04 IDL                   "}
{"type":"event","kind":"voice","talkgroup":"150607","source_id":"2222223","slot":"2","color_code":"4","ran":"","crc_error":"","extra":"","raw":"(dsdcc slot2) *04 VOX 02222223>G00150607"}
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
{"type":"event","kind":"call","talkgroup":"200","source_id":"100","slot":"","color_code":"","ran":"9","crc_error":"","extra":"","raw":"(dsdcc nxdn) RAN 9 src 100 dst 200 group"}
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
