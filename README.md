# dsd-server

A C++ server that sits between your SDR's raw IQ output and the DSD-FME
DMR decoder: it FM-demodulates IQ streamed in over a WebSocket, feeds the
resulting discriminator audio into a `dsd-fme` subprocess, and relays
decoded voice audio + call metadata (talkgroup, source ID, slot, etc.)
back out over the same WebSocket connection.

```
 SDR client                 dsd-server (this project)                dsd-fme
 ───────────                ───────────────────────────              ────────
 raw IQ  ───WebSocket──▶  FmDemodulator ──stdin PCM──▶  DsdProcess ──▶ dsd-fme
 (float32,                (FIR filter, decimate,                        │
  interleaved I/Q)         quadrature demod, resample                   │ stdout (event log)
                            to 48kHz PCM)                                │ UDP (decoded voice PCM)
                                                                          ▼
                                                          DsdProcess reads both,
                                                          Session relays back to
 client  ◀──WebSocket───  JSON events + tagged binary   ◀── client over WS
          (audio + events) audio frames
```

Each connected WebSocket client gets its own `FmDemodulator` instance and
its own dedicated `dsd-fme` child process — sessions are fully isolated,
so multiple clients can decode different channels concurrently.

It has since grown into a general **digital decoder server**: the same
WebSocket, IQ format and `event` frame also carry **TETRA** (π/4-DQPSK +
tetra-rx / tetra-kit) and **paging** (POCSAG / FLEX via multimon-ng), each
selected per session by the client's `protocol` hint. Voice audio is emitted
where a mode has it; data-only modes like paging just produce events. See
"TETRA (runtime-selected)" and "Paging (runtime-selected)" below.

## What's implemented

- `src/fm_demod.{hpp,cpp}` — the original hand-rolled streaming FM
  discriminator: NCO frequency shift (optional), windowed-sinc FIR
  channel filter, integer decimation, quadrature demod (`atan2` of
  `x[n] * conj(x[n-1])`), linear resample to exactly 48000 Hz, gain
  scaling to 16-bit PCM.
- `src/dsd_process.{hpp,cpp}` — the original subprocess-based DSD
  backend: spawns and manages a `dsd-fme` (or classic `dsd`) child
  process via `fork`/`exec`/pipes, writes PCM to its stdin, reads its
  stdout event log (regex-classified into structured `DsdEvent`
  records), and listens on a per-session UDP port for its decoded voice
  audio.
- `src/dsdcc_decoder.{hpp,cpp}` — an in-process alternative built on
  DSDcc (https://github.com/f4exb/dsdcc), a C++ library rather than a
  subprocess: samples are pushed directly into a `DSDDecoder` object and
  decoded audio is pulled back out via a getter, with no child process,
  pipes, or UDP socket involved. Structurally the most different of the
  backend swaps in this project — see "The DSDcc variant"
  below. Now verified against DSDcc 1.9.0 with a real DMR capture (it
  was originally written blind, without access to DSDcc).
- `src/dsd_backend_selector.hpp` — compile-time switch between the two
  DSD backends (`-DDSD_USE_DSDCC_BACKEND`), so `session.cpp` stays
  agnostic to which one it's linked against.
- `src/dsd_backend_types.hpp` — the `DsdEvent` type shared by both DSD
  backends, so `session.cpp` handles events identically regardless of
  which one produced them.
- `src/session.{hpp,cpp}` — one `Session` per WebSocket client, wiring
  the demod backend and DSD backend (whichever ones were compiled in)
  together, plus the JSON control protocol.
- `src/json_util.hpp` — minimal hand-rolled JSON reader/writer (the
  message schema is small and flat; swap for `nlohmann::json` if you grow
  past that).
- `src/main.cpp` — server entry point (Boost.Asio `io_context` + a small
  thread pool).

## What's been tested, and what hasn't

I don't have network access in the environment I built this in, so I
could not install Boost or compile/run the actual WebSocket server
end-to-end. To still validate the pieces that matter most:

- **`fm_demod.cpp`** — compiled standalone and run against a synthetic
  FM-modulated tone (see `tests/test_fm_demod.cpp`). Confirms the streaming
  block/history-carry logic doesn't crash across chunk boundaries and
  that the recovered discriminator magnitude lands in the theoretically
  expected range for a known injected deviation. This is a sanity check,
  **not** validation against a real DMR capture — you should verify
  against your own SDR before trusting it operationally.
- **`dsd_process.cpp`** — now verified against REAL dsd-fme
  (lwvmobile/dsd-fme built from source) decoding a real DMR capture,
  not just the fake stand-in. That verification found and fixed three
  genuine bugs in the originally guessed command line: `-f d` selected
  D-STAR (dsd-fme's DMR mode is `-fs`), `-U host:port` was the rigctl
  port rather than audio output (the real flag is `-o udp:host:port`),
  and with no `-o` at all dsd-fme exits at startup when no PulseAudio
  daemon exists — fatal on a headless server. It also fixed the
  talkgroup regex (real dsd-fme writes `TGT=`, which the old `TG`-only
  pattern never matched), slot attribution (the bracketed `[SLOT2]`
  marker is what names the active slot), ANSI color codes leaking into
  event text, and unescaped control characters producing invalid JSON.
  See the DSD-FME VERIFICATION NOTES in `dsd_process.cpp` and the two
  tests: `test_dsd_process` (DsdProcess + real dsd-fme + real capture)
  and `test_session_real_fme` (the full production stack over a real
  WebSocket — the strongest end-to-end check in the project).
- **`session.cpp` / `main.cpp`** — reviewed carefully but **not compiled**
  (no Boost available in the sandbox). I found and fixed two real bugs
  during that review that are worth knowing about even though you'll
  compile this fresh:
  1. A reference cycle where callbacks stored inside the per-session
     `dsd_` member captured a `shared_ptr` back to the owning `Session`,
     which would have leaked every session indefinitely. Fixed by
     capturing `weak_ptr` in the long-lived callbacks.
  2. A strand that wasn't actually bound to the WebSocket stream's async
     operations, which would have allowed the network thread and the
     dsd-fme reader threads to race on `ws_` under the multi-threaded
     `io_context` in `main.cpp`. Fixed by binding each accepted socket to
     its own strand at accept time (`net::make_strand`), following
     Boost.Beast's own multi-threaded server example pattern.
- **`dsd_process.cpp`** and **`session.cpp`'s use of it** — these are the
  same files described above; no changes beyond what's already noted.
- **`dsdcc_decoder.cpp`** — originally the least verified file in the
  project (written from header fragments with placeholder API calls);
  since then it has been rewritten against DSDcc 1.9.0's real headers
  and dsdccx's own integration loop, and validated end to end with
  DSDcc's bundled real DMR capture — see "The DSDcc variant" below for
  what "validated" means concretely (it's sample-exact against
  upstream's own decoder output).
- **`test_session.cpp`** — an integration test for `session.cpp` itself
  (real `Server`, real Beast WebSocket client, exercises the actual wire
  protocol). Like `session.cpp` originally was, this is reviewed but not
  compiled by me — see "Testing session.cpp" below.

**Where verification stands now**: everything above has since been
built, run, and tested — including under ThreadSanitizer (see the
concurrency test's TSan target), under AddressSanitizer + UBSan (build
with `-DDSD_ENABLE_ASAN=ON` in a dedicated build dir and run ctest; the
full suite passes with zero reports — no corruption, leaks, or detected
UB), and against real dsd-fme and real DMR
RF (see `test_dsd_process` / `test_session_real_fme` /
`test_dsdcc_decoder` / `test_session_dsdcc`). The remaining genuinely
untested surface is a live SDR as the IQ source (all RF-derived testing
uses a captured discriminator recording, FM-remodulated for the
full-stack tests).

## Build

### Docker (Debian bookworm)

The provided `Dockerfile` builds everything — including the DSP
dependencies Debian doesn't package (mbelib, DSDcc, dsd-fme, and the two
TETRA decoders `tetra-rx`/tetra-kit, each pinned to a fixed commit — the
DSD ones to the commits the backends were verified against) — and produces
a slim runtime image containing the server variants plus the decoders they
spawn:

```bash
docker build -t dsd-server .
docker run --rm -p 22600:22600 dsd-server                     # subprocess backend (default)
docker run --rm -p 22600:22600 dsd-server dsd-server-dsdcc     0.0.0.0 22600 4  # in-process DSDcc
```

TETRA is not a separate executable — every variant above decodes it from
the client's `protocol` hint: `tetra` (osmo `tetra-rx`) or `tetrakit`
(tetra-kit's `decoder`); both decoders are on the image's `PATH`. (The image
ships no ACELP voice codec — patent/GPL, see `TETRA_VOICE.md` — so TETRA
sessions emit events only.)

`docker build --target test .` additionally runs the entire ctest suite
(including the real-capture tests against the just-built dsd-fme and
DSDcc) inside the image and fails the build if anything fails — usable
as CI. On hardware that can't decode much faster than realtime (e.g. a
1.2 GHz ARMv7), add `--build-arg DSD_TEST_PACE_MS=60`: the two
full-stack session tests stream their capture at ~8.5x realtime by
default, and a box that can't keep up overflows the session's
drop-oldest IQ queue, failing those tests with truncated audio even
though the pipeline is fine at realtime.

`docker build --target loadtest -t dsd-server-loadtest .` builds a
capacity-measurement image; `docker run --rm dsd-server-loadtest` then
measures 8 concurrent realtime 32 ksps streams against the DSDcc
backend **on whatever machine the container runs on** and prints the
measured CPU per stream plus a streams-per-box estimate — the intended
way to get real capacity numbers on deployment hardware (e.g. an ARM64
box). Arguments override the defaults: server binary, BLUE file
(mount your own with `-v`), stream count. See the loadtest stage in
the Dockerfile.

### Native

Dependencies: a C++17 compiler, CMake ≥ 3.16, Boost ≥ 1.74 (headers +
`boost_system`), and `dsd-fme` built/installed separately
(https://github.com/lwvmobile/dsd-fme).

```bash
# Debian/Ubuntu
sudo apt install build-essential cmake libboost-dev libboost-system-dev

mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j
./dsd-server            # listens on 0.0.0.0:22600 by default
```

To run just the DSP sanity test (no Boost needed):
```bash
make test_fm_demod
./test_fm_demod
```

## The DSDcc variant

`dsdcc_decoder.cpp` swaps out the *DSD backend* (not the demod) —
instead of spawning a `dsd-fme`/classic-`dsd` subprocess and talking to
it over pipes and a UDP socket, it links DSDcc
(https://github.com/f4exb/dsdcc) directly and pushes samples into a
`DSDDecoder` object in-process. This is a structurally bigger change
than the subprocess backend: no child process, no pipes, no
UDP audio port. If it works out, it's also a more direct answer to
running many concurrent sessions on constrained hardware — 16 sessions
means 16 decoder *objects* in your
existing threads instead of 16 forked OS processes competing for cores.

**Status: verified against DSDcc 1.9.0, end to end, with a real DMR
signal.** This file was originally written blind (no network access to
DSDcc), with its API calls marked LOW CONFIDENCE and its extraction
logic stubbed out. It has since been rewritten against the real
`dsd_decoder.h`/`dmr.h` and against `dsd_main.cpp` (upstream's own
`dsdccx` CLI, the canonical integration example), and validated with
DSDcc's bundled discriminator capture of a real DMR transmission
(`samples/dmr_it_8.dis` in the DSDcc source tree). The verification is
strong: running the capture through `DsdccDecoder` produces **exactly**
the decoded audio upstream's `dsdccx` produces from the same file
(151680 samples of 8 kHz voice, sample-count-exact), with the correct
talkgroup (150607), sources, group-call flag, and TDMA slot in the
emitted events.

For the record, how the original blind guesses fared (also in
`dsdcc_decoder.cpp`'s top comment): `run(sample)` per input sample was
guessed exactly right, and the `<dsdcc/...>` include prefix was right;
the audio getters were close (right idea, wrong access path — it's
`getAudio1/2()` on `DSDDecoder` directly, one per TDMA slot); but the
guessed `setDecodeMode(mode, false)` second argument was backwards —
the bool means on/off, so the placeholder would have silently
*disabled* DMR decoding. The metadata getters didn't exist as guessed;
the real source is DSDcc's fixed-layout 26-char per-slot status text,
which `dsdcc_decoder.cpp` now parses (layout documented there,
confirmed against `dmr.cpp`).

Two contracts worth knowing: DSDcc's input rate is **fixed at 48 kHz**
(S16LE discriminator audio; `start()` rejects anything else rather than
silently failing to sync), and decoded audio comes out at **8 kHz**
(DSDcc's MBE decoder native rate, upsampling off — the same rate
dsd-fme's UDP output typically uses, so clients see no difference).
`session.cpp` needed zero changes to host this backend — its "always
post to the connection's strand" callback pattern already covered
callbacks firing synchronously on the demod worker thread.

**Getting this running:**

1. Build and install mbelib (github.com/szechyjs/mbelib), then DSDcc
   (github.com/f4exb/dsdcc) — both are plain CMake builds.
2. `cmake .. && make dsd-server-dsdcc` — CMake skips this variant with a
   message (not an error) if DSDcc/mbelib aren't found.

**Testing it** (this is no longer the untested backend — it's the most
thoroughly tested one):

- `test_dsdcc_decoder` runs `DsdccDecoder` directly on the real DMR
  capture and asserts the decoded audio volume and the exact
  talkgroup/source/slot metadata, plus rejection of bad configs.
- `test_session_dsdcc` is the full-stack version: it starts the actual
  WebSocket server built with this backend, FM-modulates the capture
  into IQ on the client side, streams it over the socket, and asserts
  that real decoded voice and the real talkgroup come back out. A pass
  means IQ → demod → DSDcc → WebSocket worked on genuine RF-derived
  data end to end (also sample-exact: all 151680 voice samples arrive).
- Both are armed in ctest by pointing CMake at a DSDcc source checkout:
  `cmake -DDSDCC_SAMPLES_DIR=/path/to/dsdcc/samples ..`. Without that
  they build but print SKIPPED, since the capture ships in DSDcc's
  source tree, not its installed artifacts.

## TETRA (runtime-selected)

TETRA is π/4-DQPSK, not an FM mode, so it needs its own front end
(`src/tetra_demod.*`, a streaming π/4-DQPSK modem) feeding an external
decoder. It is **not a separate executable**: the one `dsd-server` binary
carries this chain alongside the FM/DSD chain and picks it per session from
the client's `protocol` hint (see the Protocol table above). The FM/DSD
backend choice (dsd-fme vs DSDcc) is still build-time; the TETRA
*decoder* choice is runtime:

- **`protocol":"tetra"`** — spawns osmo-tetra's `tetra-rx` (sq5bpf fork);
  events over its TETMON protocol. Surfaces the control plane
  (`NETINFO1`/`FREQINFO1` → `sync`, call-control PDUs → `call`).
- **`protocol":"tetrakit"`** — spawns tetra-kit's `decoder`; JSON reports.
  Surfaces the traffic channel (`TCH_S` → `voice`).

Both are selectable from either server variant (`dsd-server`,
`dsd-server-dsdcc`) since the TETRA stack compiles into
each. Both are **validated end to end on a real off-air capture**: our demod
locks the burst grid, and the bits, fed to the real decoders, decode
coherently (UK network, MCC/MNC 234/78). Voice speech frames are extracted
(base64+zlib) but the ACELP codec is a documented external plug-in
(patent/GPL, not vendored) — `TETRA_VOICE.md` has the design and the
end-to-end validation. `PROTOCOL.md`'s TETRA section is the wire reference.
zlib is a hard dependency of every server build (it backs the tetra-kit
speech extraction); the decoders themselves must be on `PATH` at run time,
and the `Dockerfile` bundles both. A session started with a TETRA `protocol`
whose decoder isn't installed replies with an `error` frame and stays open.

**Detection mode (coherent vs. differential).** TETRA sessions default to
**Costas coherent detection**, which decides each symbol against a recovered
carrier reference for **≈1.5–1.7 dB** better BER in the mid-SNR range (measured
in `tests/test_tetra_demod.cpp`). π/4-DQPSK's even/odd constellation parity —
which coherent detection must know — is resolved automatically from burst-grid
lock (`src/tetra_frontend.*`), and if it can't lock (non-TETRA or too-weak
signal) the session **falls back to differential on its own** — so the robust
phase-blind path is never lost, it's the safety net. Coherent carries a low-SNR
crossover (~1.3 dB Eb/N0) below which the loop can slip; set
`DSD_TETRA_COHERENT=0` in the server's environment to force plain differential
detection there. The `tetra_bit_source` bridge tool exposes the same choice via
`--differential`. Validated on the real off-air capture: coherent auto-picked
the right parity, locked the same 18-burst grid, and — through the real
`tetra-rx` — decoded the same network (MCC/MNC 234/78, ColorCode 0x17) while
recovering ~5% more CRC-protected control-plane messages than differential
(see PROTOCOL.md).

## Paging (runtime-selected)

POCSAG and FLEX pagers are decoded by a third runtime chain, picked per
session by the `protocol` hint:

| hint | decoders |
|---|---|
| `pager-auto` | POCSAG 512 + 1200 + 2400 and FLEX, all at once |
| `pocsag` / `pocsag512` / `pocsag1200` / `pocsag2400` | POCSAG, all or one rate |
| `flex` | FLEX (1600/3200/6400 bps, 2/4-level) |

(Plain `auto` is still the DSD auto-detect; paging is only selected by these
hints.) The chain is `src/pager_demod.*`, an FM discriminator that produces
the 22050 Hz audio multimon-ng expects, feeding `src/pager_process.*`, a
per-session [multimon-ng](https://github.com/EliasOenal/multimon-ng) child run
with `--json`. `src/pager_events.*` maps its JSON onto the ordinary `event`
frame:

```json
{"type":"event","kind":"page","talkgroup":"1523020","source_id":"","slot":"","color_code":"","ran":"","nac":"","emergency":"","alias":"","crc_error":"0","message":"Passage Ambulance Wilhelminabrug Leiden","extra":"protocol=flex; baud=1600; message_type=alpha; flex_type=alphanumeric; levels=2; phase=A; cycle=14; frame=118; addr_type=S; group=0; fragment=complete","raw":"{...multimon-ng's JSON...}"}
```

- `talkgroup` is the pager address (POCSAG RIC / FLEX capcode).
- `message` is the page text.
- `emergency` flags a FLEX priority page, and `crc_error` a failed FLEX
  checksum.
- `extra` carries protocol, baud, message type and the FLEX frame details.
- Pages whose text looks like ciphertext (encrypted "secure paging") are
  still emitted, since the capcode is valid, with
  `payload=encrypted_or_binary` in `extra` so clients don't display them as
  text.
- FLEX network broadcasts (date/time/system id) arrive as `kind:"sync"`.

The same `start` message works. `sample_rate`, `channel_bandwidth`,
`freq_offset` and `afc` apply, plus two paging-only fields:
- `pocsag_mode` (`auto`/`alpha`/`numeric`/`skyper`);
- `invert` (for spectrally inverted IQ).

`gain`, `matched_filter` and keys are ignored: the paging demod scales its
output by deviation in Hz, so its level is right at any IQ rate. Full details
are in PROTOCOL.md "Paging".

**Decoder requirement:** a multimon-ng **newer than 1.3.0**, because it needs
`--json` and the `FLEX_NEXT` decoder, and the Debian/Ubuntu packages are too
old. The Docker image builds it from source at a pinned commit. To build it
natively:

```bash
git clone https://github.com/EliasOenal/multimon-ng && cd multimon-ng
git checkout 0722194b7739748e49f18ac1fc76f236d4ca390d
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DX11_SUPPORT=OFF -DPULSE_AUDIO_SUPPORT=OFF -DSDL3_SCOPE=OFF
cmake --build build -j && sudo install -m755 build/multimon-ng /usr/local/bin/
```

The server finds it on `PATH`, or at `$MULTIMON_NG`.

Two process details matter for multimon-ng:
- **Output buffering.** multimon-ng never flushes its JSON output. On a pipe,
  FLEX pages would sit in a 4 KiB buffer until the session stopped, so the
  child runs under coreutils `stdbuf -oL`. Set `PAGER_NO_STDBUF=1` to disable
  that.
- **Inherited sockets.** Every decoder child closes all inherited fds before
  `exec`. That covers dsd-fme, tetra-rx, tetra-kit and multimon-ng alike (see
  `src/child_fds.hpp`). Asio's sockets aren't close-on-exec, and without this
  each decoder kept copies of the listening socket and of other sessions'
  WebSocket connections alive after they closed. `test_session` checks this
  for every backend.

**Tests:**
- `test_pager_demod` checks the demod against synthetic FSK.
- `test_pager_events` checks the JSON → event mapping against real
  multimon-ng output.
- `test_session_pager` runs the full stack with the real multimon-ng. It
  streams FM-modulated IQ built from gen-ng synthetic pages and multimon-ng's
  bundled **off-air** recordings: POCSAG at all three rates, and a 51 s
  capture of the Dutch P2000 FLEX network with 46/46 pages recovered. Every
  page a direct multimon-ng decode finds must come back live over the
  WebSocket. It reports skipped unless multimon-ng and the fixtures are
  present:

```bash
tools/make_pager_fixtures.sh /path/to/multimon-ng build/pager_fixtures   # needs its build/gen-ng + sox
cmake -S . -B build -DMULTIMON_NG_BIN=/path/to/multimon-ng/build/multimon-ng
cmake --build build --target test_session_pager && (cd build && ctest -R pager)
```

Not yet verified: a live SDR as the source, and 3200/6400 bps 4-level FLEX
(no capture or generator was available).

## Testing session.cpp

`test_session.cpp` is an integration test for the WebSocket server
itself — not a demod or DSD-backend test. It starts a real `Server` on a
local port, connects a real Boost.Beast WebSocket client to it (same
process, separate thread), and drives the actual documented wire
protocol: sends `start`/`stop`/malformed messages, sends synthetic IQ
binary frames, and checks the responses. It uses a small stand-in
`dsd-fme` (`test_fake_dsd_fme.cpp`, portable C++, no Python dependency)
so it doesn't need a real DSD binary or a real DMR signal — the fake
just proves data actually flows end-to-end through `FmDemodulator` →
`DsdProcess` → back out over the WebSocket, with plausible-looking
synthetic events and audio.

**What it checks:**
- `start` produces the documented `{"type":"started",...}` response.
- Malformed JSON and unrecognized message types get
  `{"type":"error",...}` instead of crashing or hanging the connection.
- A binary frame with a length that isn't a multiple of 8 bytes gets an
  error response (after `start`).
- Binary IQ frames sent *before* `start` are silently dropped, not
  errored — matching `handle_binary_message`'s documented behavior.
- Sending real (synthetic) IQ frames after `start` produces both a JSON
  `event` frame and a tagged binary audio frame back — i.e. the whole
  pipeline wired up in `session.cpp` actually moves data, not just that
  individual pieces compile.

**What it doesn't check:** real DMR decoding (the fake `dsd-fme` doesn't
care what audio it receives), the DSDcc backend
specifically (this always builds against the default `FmDemodulator` +
`DsdProcess`), or concurrency — it's a single client, single session,
sequential test cases, deliberately kept simple. A stress test with many
concurrent clients would be a good next addition, particularly given the
strand-binding fix described earlier in this README — that's exactly
the kind of bug a single-client test like this one structurally can't
catch.

**Honesty note, consistent with everything else in this project:** this
is, by a wide margin, the most complex file here that I haven't been
able to compile myself — same situation `session.cpp` was in originally,
before you built it and found the real `json::Writer` copy-constructor
bug. I reviewed the Beast client API calls carefully, and they mirror
patterns already used server-side in `session.cpp` (which by now has
been through a real compiler), but that's reassurance, not verification.
If it doesn't compile cleanly on the first try, that would track with
this project's history so far — send me the errors.

**Building and running:**

```bash
cd build
cmake ..                    # picks up the new test_session/test-fake-dsd-fme targets
make test_session            # also builds test-fake-dsd-fme and copies it to "dsd-fme"
ctest -R session_integration_test --output-on-failure
# or, equivalently, run it directly:
TEST_FAKE_DSD_FME_DIR=$(dirname $(find . -name test-fake-dsd-fme)) ./test_session
```

If port 18765 (hardcoded in `test_session.cpp`'s `kTestPort`) is already
in use on your machine, every test will hang rather than fail fast —
`Server`'s constructor doesn't currently report bind failure back to the
caller, it only logs to stderr (see the comment at that constant's
definition). Check for a `bind failed` line in stderr before assuming a
hang means something else is wrong, or just change the port.

## Running

```bash
./dsd-server [listen_address] [port] [threads]
# e.g.
./dsd-server 0.0.0.0 22600 4
```

Make sure `dsd-fme` is on `PATH`, or edit `DsdProcessConfig::dsd_fme_path`
in `session.cpp` to an absolute path.

**Verify the `dsd-fme` CLI flags for your installed version** — they've
changed across releases/forks. `build_argv()` in `dsd_process.cpp` and
the stdout-parsing regexes in `classify_line()` are both marked with
comments pointing at exactly what to check against `dsd-fme -h` and your
own captured log output.

## Status page

The same listening port serves a small HTTP status page for a plain
(non-WebSocket) GET, so the server is both the WebSocket endpoint and its
own dashboard — no extra port. A browser upgrade request becomes a client
session exactly as before; anything else is answered over HTTP:

| Path | Response |
|---|---|
| `/`, `/status` | HTML dashboard (live-updates ~1 s by polling the JSON in place; falls back to a 5 s full-page `<meta>`-refresh if JavaScript is off) |
| `/status.json` | the same data as JSON, for health checks / scraping |
| `/iq_log/on`, `/iq_log/off` | flip the global IQ-capture switch (the page's **Log IQ** checkbox); returns `{"iq_log_enabled":…}` |
| `/net` | the network explorer page (see [Network explorer](#network-explorer)) |
| `/net.json`, `/net/clear` | the explorer's association model as JSON; forget it (and its imports) |
| `/net/export.json`, `/net/export.graphml` | export what the explorer shows: native (re-openable) / GraphML |
| `POST /net/import`, `/net/imports/remove`, `/net/imports/clear`, `POST /net/merge` | add exports to the live view, remove them; merge exports without the live data |
| `/net/log/on`, `/net/log/off`, `/net/log/download` | record the explorer's inputs (`?clear=1` clears first); stop; download the current / last recording |
| `/net/audio/on`, `/net/audio/off`, `/net/audio/<file>.wav` | record each call's decoded voice; stop; play a call's audio |
| `/net/asr/config.json`, `/net/asr/<file>`, `/net/asr_worker.js` | speech-to-text: what the asset folder holds; its files (library, model); the browser worker |

The live update is a tiny `/status.json` poll that patches the page in
place — cheap on the server (no decode work, just a mutex-guarded snapshot
and a few hundred bytes) and flicker-free in the browser.

Both show the number of **currently connected** sessions, the
**cumulative total** since the server started, how many are **actively
decoding**, a per-protocol breakdown of the active ones, and a table of
each live client (id, peer address, decode protocols, idle vs decoding,
connect time and duration). A session's protocol appears
once it sends `start`; before that it shows `-`. HTTP status requests are
**not** counted as sessions.

The HTML page has four tabs:
- **Sessions:** the live clients above.
- **Protocols:** a run-wide record of every protocol clients have asked for
  since the server started, not just what's running now. For each protocol:
  whether it's active now, requests / started / failed counts, distinct
  sessions, total decode time, and first/last request time.
- **History:** the most recent finished sessions (newest first, last 50 by
  default) with their connect/end times and total duration.
- **Log:** described below.

In Sessions and History, the protocol column lists every protocol a client
requested on that connection (e.g. `dmr → nxdn48 ✗ → pager-auto`). ✗ marks a
request whose pipeline failed to start.

The **Log** tab is a
live view of the JSON frames the server has sent clients (last 300, newest
first), with **Pause** (freeze the view for inspection) and **Clear**
(empty the server's log buffer) buttons. Binary
voice audio, the high-rate `kind:"voice"` events, and the once-per-connect
`capabilities` greeting are excluded; everything else
(`started`/`error`/metadata events) is shown. Protocols, History and Log are in-memory only and reset on restart;
the JSON exposes them as `protocols` and `history` arrays, and the Log via a
separate `/log.json` endpoint.

Above the tabs is a **Log IQ to BLUE file** checkbox — a global switch that
turns raw-IQ capture on or off for every session live (see below).

```bash
curl http://localhost:22600/status.json
# open http://localhost:22600/ in a browser for the live view
```

## Network explorer

> **User manual:** [docs/NET_EXPLORER.md](docs/NET_EXPLORER.md) — a tour of
> the page, every view and control, troubleshooting, and appendices on how
> calls and associations are worked out for each protocol. A printable copy
> to hand out is [docs/NET_EXPLORER.pdf](docs/NET_EXPLORER.pdf) (rebuild it
> after editing the manual with `tools/build_manual_pdf.py`; it needs Python's
> `markdown` package and Chrome or Chromium). This section is the
> server-side reference.

`/net` on the same port is a second page that turns the decoded event stream
into **calls, talkgroups, radios and networks, and the associations between
them** -- live, per protocol (P25, DMR, NXDN, TETRA, dPMR, D-STAR, YSF, EDACS).
Protocols are never cross-linked: a P25 radio id and a DMR radio id are
unrelated numbers.

- **Calls** -- one row per call, stitched from the per-frame events: source
  (with talker alias) -> talkgroup, or radio -> radio for a private call;
  voice vs. data/SMS (with the text), slot, emergency, encrypted, duration,
  and a live indicator. A call heard by two receivers (e.g. a P25 control
  channel's grant and the voice channel) is one call, marked `2 RX`.
- **Talkgroups / Radios** -- who talks on what, how often, on which networks.
  Click any radio, talkgroup or network for its associations: the talkgroups
  a radio uses, its private-call partners, the radios it shares talkgroups
  with, the talkgroups linked to a talkgroup through shared radios, and
  recent calls.
- **Graph** -- a force-directed radio <-> talkgroup graph (private calls as
  dashed links, colour = network, a white ring = seen on 2+ networks).
- **Links** -- the analysis view: *talk communities* (groups of radios tied
  together through shared talkgroups / private calls), talkgroups and radios
  **seen on more than one network** (evidence of a link between systems),
  and *hub radios* active on 3+ talkgroups (dispatchers, supervisors,
  scanning radios).
- **Networks** -- each identified network with its identifiers and sites.

The page works on tablets and phones as well as a PC. On a touch screen
buttons, tabs and rows are finger-sized and the page scrolls as one (no
scroll boxes inside it). Below 1050 px wide the details open in a drawer
(a bottom sheet on a phone) with a Close button, instead of below the
table. On a phone the header's actions fold into a **Menu**, calls are
listed as cards, other tables stack into cards with a sort menu, and the
stat cards shrink to a strip. The graph has pinch-zoom and -/+ buttons and is
sized to leave room to scroll past it. Explanations that a mouse shows on
hover (what "2 RX" or an identity level means) appear on tap. A PC window
wider than 1050 px looks as before.

**How networks are identified.** Identity arrives on different lines than
calls, so each stream keeps the identity it has decoded and its calls are
attributed to it:

| Protocol | Strong identity | Weak identity (short code) | Site |
|---|---|---|---|
| P25 | WACN + System ID | NAC | RFSS + Site |
| DMR | Network ID (Tier III / Capacity Max) | color code | Site ID |
| NXDN | System code | RAN | site code / location |
| TETRA | MCC + MNC | colour code | location area |

A weak id is a short code that unrelated systems routinely share (a DMR color
code has 16 values), so a weakly identified stream gets its own bucket
(`Color Code 1 · stream 3`) instead of being merged with other streams on
the same code; talkgroups and radios it shares with other buckets show up
under **Links** as evidence instead. **Unless the channel frequency is
known:** when the client's `start` carries `center_freq` (the tuner centre;
the channel is `center_freq + freq_offset`), a short code on a channel is in
practice one conventional channel or repeater, so the bucket is the channel's
(`Color Code 1 · 434.4250 MHz`, identity `channel`). Every session on that
frequency with that code -- a reconnect, a restart, a second receiver --
lands in the same network, and a call two sessions heard there is one call.
Calls show their frequency (the Calls view's **MHz** column; the search box
matches it) and networks list the channels they were heard on. A stream with
no code decoded yet is `Unidentified · 434.4250 MHz`. The status page still
lists sessions by session number. Frequencies are snapped to
`DSD_NET_FREQ_STEP_HZ` (default 1250 Hz -- the 6.25 kHz and 2.5 kHz channel
plans stay exact and an offset up to ±625 Hz off still lands on its channel);
a `set_freq_offset` moves the stream to the new channel. P25's NAC is the exception: a stream
that only hears a NAC is resolved to the known WACN/SysID network carrying
that NAC, if exactly one does. Neighbour-site broadcasts are recorded as
sites without moving the stream's identity, and a short code must be seen
twice in a row before it is believed (dsd-fme prints placeholder values such
as `Color Code=00` before a burst's code is decoded).

A stream only shows up once it decodes real traffic (a sync, call, voice or
data burst): a session on an empty channel, on noise, or set to the wrong
protocol leaves no trace -- no protocol tab and no "Unidentified" network.
When a session ends (stop, disconnect, or a new `start`), any network it fed
that never carried a call is dropped too, along with radios known only through
it -- unless another running session is still on that network.

Everything is in memory, bounded, and resets on restart or with the page's
**Clear** button. `GET /net.json` serves the same model for tooling (gzip-
compressed when the client accepts it).

- **Calls kept:** the newest 5000 per protocol (`DSD_NET_MAX_CALLS`). When the
  list is full the oldest call goes -- but calls with recorded audio last: the
  oldest call *without* audio is dropped first. Each extra 1000 calls costs
  roughly 1 MB of JSON per explorer refresh (about 100 KB compressed), so
  lower it on a slow link or a busy multi-protocol server.
- **Calls / s:** the stat card shows calls per second over the last minute
  (hover / tap for the 10-minute average and the total since start). It is
  counted from every call, not just those still listed; with a network or
  search filter it is worked out from the listed calls.
- Radios, talkgroups and networks are capped too (3000 / 1500 / 200 per
  protocol, least recently heard dropped first).

### Exporting and re-opening explorer data

**Export ▾** in the explorer header saves what the explorer currently shows:

- **Explorer data (.json)** -- the native export (`format: "dsd-net-export"`,
  `format_version: 1`): networks with their identifiers and sites, talkgroups,
  radios, every association with its call count, and the recent calls. Open it
  again later with **Open...** (or drop the file on the page): the explorer
  shows it offline in a read-only file view, with every view -- calls,
  graph, links, details -- working, and times shown relative to the export.
- **Association graph (.graphml)** -- for graph tools (Gephi, Cytoscape, yEd,
  networkx). Radios, talkgroups and networks are nodes (attribute `type`,
  `protocol`, `label`, `calls`, aliases, identifiers, first / last seen); edges
  are radio-talkgroup (`type=talkgroup`, `weight` = calls), radio-radio
  (`type=private`) and talkgroup/radio-network (`type=member`). Protocols
  never share an edge.

The export is *not* the same as a recording: a recording is the raw decoder
input (for debugging -- it reproduces the explorer exactly), while an export is
the gathered result (compact, opens directly). A recording can be turned into
either export with `net-replay <recording> --export out.json --graphml
out.graphml` (add `--at +N` for the state N seconds in). Exports carry the
bounded recent-calls list (last 400 per protocol) but the full associations
and counts. `GET /net/export.json` and `GET /net/export.graphml` serve them
directly.

### Combining receivers: merging and importing exports

Exports from several receivers (or from one receiver at different times)
can be combined:

- **Open...** with several files (or drop several on the page) shows them
  merged, read-only, with what happened to each file and a **Save merged**
  link. The live data is not touched.
- **Import...** adds exports to the *live* view: the page shows the live
  data and the imports together, with a bar listing the imports (each
  removable). New traffic keeps arriving on top. Export then saves the
  combined view; Clear also drops the imports.
- `net-merge a.json b.json.gz ... -o merged.json [--graphml merged.graphml]`
  does the same from the command line (built alongside the server).

Talkgroups and radios are joined by id within a protocol, and networks by
their strong id (P25 WACN/SysID, DMR network id, NXDN system code, TETRA
MCC/MNC) -- so two receivers on the same system become one network with both
receivers' radios. Channel networks (a short code on a known frequency) join
too: `Color Code 1 · 434.4250 MHz` from two receivers is one network. If your
receivers are far enough apart to hear *different* systems on one frequency,
run with `DSD_NET_CHANNEL_MERGE=receiver` (`net-merge --per-receiver`) to keep
each receiver's apart (its runs still join). A weak id with no frequency (a
DMR color code, a bare NAC or RAN) can't tell systems apart, so each
receiver's stays its own network, labelled with the receiver's name
(`Color Code 1 · stream 3 · rx-north`), and shared talkgroups and radios show
the link under **Links**. A call both receivers heard on the same system or
channel is shown and counted once (`2 RX`).

Each export records which server run it came from and the time span it
covers, so the merge never counts the same calls twice: the same file twice,
or an older export of a run next to a newer one, is merged once (the newer
replaces the older); a file that only partly overlaps another is refused,
with the reason; importing a server's own export into it is skipped. Data
from before and after a Clear are separate spans and merge fine. Name each
receiver with `DSD_SERVER_NAME` (default: the host name) so its networks are
easy to tell apart.

### Call audio

The explorer can keep each call's decoded voice and play it back: switch on
**♫ Audio** in the explorer header (in the **Menu** on small screens), or start
the server with `DSD_NET_AUDIO=1`. It is **off by default**. Calls with audio
then get a **▶** button (with their length) in the Calls view and in a radio's
or talkgroup's recent calls.

- Each call is one WAV file (8 kHz, mono, 16-bit). On DMR each TDMA slot is
  recorded separately, so two calls at once each get only their own audio;
  audio heard just before the call was decoded (up to 1 s) is included; when
  two receivers hear the same call, one recording is kept.
- No empty files: a recording starts only with audible audio (silence or
  near-silence never creates one), and one shorter than 0.2 s is deleted
  when its call ends. Calls without voice (SMS, data) never get a file.
- **Encrypted calls are never recorded** (unless the session was started with
  the key, so the call is heard in the clear). A call found to be encrypted
  after its audio began loses the file.
- Files go to `DSD_NET_AUDIO_DIR`, else `net_audio/` under the recording
  directory (`/captures/net_audio` in the Docker image). The total is capped
  by `DSD_NET_AUDIO_MAX_MB` (default 1024 MB, about 18 hours of speech): the
  oldest files are deleted first (and calls with audio stay in the explorer's
  call list longest). `DSD_NET_AUDIO_MAX_AGE_H` also deletes files
  older than that many hours. Files from earlier runs in the directory count
  toward the cap and stay playable.
- Data-only calls (SMS, private data) never get audio: while one DMR slot
  carries voice, dsd-fme copies it to both channels, and a copy that lands on a
  data call on the other slot is given to the voice call it came from.
- Audio stays on the server that recorded it: exports note which calls had
  audio, but imports and merged views have no audio to play. Explorer
  recordings (for replay) don't include audio either.

Rules on keeping intercepted communications differ from place to place;
check what applies to you before switching it on.

In the **Calls** view:

- **⤓** next to a call's ▶ downloads its WAV, named after the call
  (`call_<UTC>_<MHz>MHz_s<slot>_TG<to>_from_<from>.wav`). **⤓ Audio (.zip)**
  downloads the audio of the calls listed (in the order shown -- filter,
  search or sort first) up to 25 MB, with a `calls.csv` of when, where, who
  and any transcript.
- **Pause list** holds the list still while you read or play calls; it keeps
  updating the calls shown and counts the new ones until you press
  **Resume list**. (The header's **Pause** freezes the whole page.)
- Click the **Audio** column header to put calls with audio first, or tick
  **With audio only**.
- The call playing stays in a bar at the bottom of the window (time, channel,
  who, progress, download), however fast the list moves.

### Speech-to-text

With **Transcribe on play** ticked (the Calls toolbar), playing a call also
turns its speech into text: it shows in the bar at the bottom and then on a
line under the call, and goes into the audio zip's `calls.csv`. It is done
**in the browser**, with OpenAI's Whisper (the `small` model by default) run by
[Transformers.js](https://github.com/huggingface/transformers.js) -- the server
does no speech work; it only serves the files:

    tools/get_asr_assets.sh            # ~280 MB into ./net_asr (small)
    tools/get_asr_assets.sh small base # also base: ~3x faster, but often wrong on radio voice

Run it where dsd-server runs (or copy the folder there) and point the server
at it with `DSD_NET_ASR_DIR` -- else it looks in `net_asr/` under the
recording directory (`/captures/net_asr` in the Docker image). The script
needs `registry.npmjs.org` and/or `huggingface.co`; models come from Hugging
Face (Xenova's ONNX conversions), else an npm mirror of the same files. With
no files on the server the page offers to load them from the internet
(jsDelivr, Hugging Face) instead -- only if you choose to.

- **Which model.** Decoded radio voice (AMBE / IMBE vocoders, 8 kHz) is hard
  for the small models: on a test call of someone counting 7 to 12, `small`
  gave "Seven, eight, nine, ten, eleven, twelve.", `base` looped on "I'm
  telling you, ..." and `tiny` gave "Yeah, that's a lot." The explorer uses
  the most accurate model in the folder (`small.en` > `small` > `base.en` >
  `base` > `tiny`); the toolbar's model list (shown when there are several)
  picks another in that browser. `.en` models (from Hugging Face) are English
  only and usually a little better at it -- copy a downloaded
  `whisper-small.en` folder into `net_asr/models/Xenova/` to use one.
- **Speed.** The first call of a visit also loads the model (`small` about
  250 MB, `base` 80 MB; downloaded from the server once, then revalidated).
  Then, measured on a 4-core cloud VM, per call: `small` about 12-17 s where
  the browser allows several threads, 25-30 s where it doesn't; `base` 3-5 s /
  7-11 s. A desktop PC is usually 2-3x faster. Whisper's cost is per call, not
  per second of audio. Browsers allow threads only on pages that are HTTPS or
  `localhost`: over plain `http://<server-ip>` it runs on one thread (in
  Chrome, `chrome://flags/#unsafely-treat-insecure-origin-as-secure` with the
  server's address lifts that).
- **Language.** The toolbar's language list (default English, or
  `DSD_NET_ASR_LANG`; "Detect language" guesses, poorly on short calls).
  `.en` models are English only. `DSD_NET_ASR_MODEL` picks the default model
  when the folder has several (also a toolbar choice).
- **Accuracy.** Fine on clear speech, unreliable on short or vocoder-mangled
  calls; Whisper also invents text for silence or noise ("you", "Thanks for
  watching") or gets stuck repeating a phrase -- the explorer drops those. Treat
  transcripts as a hint, not a record.
- Transcripts are kept in that browser (`localStorage`, the last ~1000), not
  on the server; a call still in progress is transcribed again when replayed.

### Recording explorer data for analysis

The explorer can record **everything its association model receives** -- every
decoded event (including the lines clients never see), every session start /
stop, with the exact timestamps the model used -- so a live test can be
replayed offline and reproduce precisely what the explorer showed.

- **From the page:** click **Record** in the explorer header (if the explorer
  already has data it offers to clear first -- recommended, so the replay is
  exact). The header shows `REC net_<UTC>.jsonl.gz` and its size; click
  **Stop recording**, then **Download** to save the file from your browser.
- **From startup:** run the server with `DSD_NET_LOG=1` to record from the very
  first event. Stop / download with the page or `GET /net/log/off` and
  `GET /net/log/download`.
- **Where:** `DSD_NET_LOG_DIR`, else the IQ-capture directory (`DSD_IQ_LOG_DIR`
  -- `/captures` in the Docker image), else the working directory. Files are
  gzip-compressed JSON Lines (roughly 10x smaller than the raw data); each is
  capped by `DSD_NET_LOG_MAX_MB` (default 1024 MB uncompressed) and flushed
  about once a second, so even an abrupt server stop leaves a readable file.

To analyse a recording, replay it with the `net-replay` tool (built alongside
the server):

```bash
./build/net-replay net_20261001_214359.jsonl.gz            # summary + exactness check
./build/net-replay net_20261001_214359.jsonl.gz --at +42   # state 42 s into the recording
./build/net-replay net_20261001_214359.jsonl.gz --json out.json   # the replayed /net.json
```

A recording that started with an empty (or cleared) explorer is checked
against the snapshot taken when it stopped; `check OK` means the replay
reproduced the explorer exactly, so a fix to the model can be re-run on the
same data and compared.

## IQ capture (recording the raw stream)

The server can tee a session's raw IQ — the exact interleaved little-endian
`float32` the client streams — to a **MIDAS BLUE** file (type 1000, format
`CF`), a bit-exact recording you can replay with
`tools/midas_ws_client.py` or hand to any BLUE-aware tool. Handy for
capturing a signal that's misbehaving so it can be reproduced offline.

Two ways to turn it on:

- **Per session:** the client sets `iq_log:true` in its `start` message.
  The `started` reply then carries `iq_log_file` with the server-side path.
- **Live, from the status page:** tick the **Log IQ to BLUE file** checkbox
  (or `GET /iq_log/on`). Every active session starts capturing immediately,
  mid-stream, and any session that starts while it's on captures too;
  unticking it (`/iq_log/off`) finalizes every file. This is the quick way
  to grab a sample of whatever is on the air right now without touching the
  client.

Files are named `iq_<UTC-timestamp>_s<session>_<protocol>_<rate>Hz.blue`
(plus `_c<centre>Hz` when the client sent a `center_freq`).
Two environment variables tune capture (server-side):

| Variable | Default | Meaning |
|---|---|---|
| `DSD_IQ_LOG_DIR` | `.` (cwd); **`/captures` in the Docker image** | Directory the `.blue` files are written to (created if missing). |
| `DSD_IQ_LOG_MAX_MB` | `1024` | Per-session size cap in MiB; on reaching it capture stops on a sample boundary (the session keeps running). `0` = unlimited. |

Capture is **off by default** and writes a lot of data at SDR rates, so
enable it only while grabbing a specific sample.

In the Docker image this is pre-wired: `DSD_IQ_LOG_DIR` defaults to
`/captures`, a writable volume owned by the `dsd` user, so capture works
without extra env vars — just bind-mount it to get the files on the host:

```bash
docker run -v "$PWD/caps:/captures" -p 22600:22600 dsd-server
# captured .blue files appear in ./caps on the host
```

(If the bind-mounted host dir isn't writable by the container's `dsd`
uid, `mkdir -p caps && chmod 777 caps` first. To capture somewhere else,
override `-e DSD_IQ_LOG_DIR=/some/other/mounted/path`.)

## Test client: MIDAS BLUE files

`tools/midas_ws_client.py` is a ready-made client for feeding the server
from an X-Midas BLUE file: it parses the (attached) header, extracts
complex IQ — formats CB/CI/CL/CF/CD, either endianness, sample rate from
`xdelta` — streams it over the WebSocket, prints every JSON frame the
server sends to stdout, and writes the decoded audio to a WAV file
(8 kHz; stereo/mono inferred automatically from which backend answered).
Stdlib-only Python 3 — no pip installs.

```bash
python3 tools/midas_ws_client.py capture.tmp --port 22600 --wav out.wav --afc
python3 tools/midas_ws_client.py capture.tmp --info   # just dump the header
```

Useful knobs: `--speed N` (streaming pace as a multiple of realtime,
default 4; 0 = unpaced), `--freq-offset`, `--gain`, `--sample-rate`
(override the header), `--channels` (override the WAV inference), and
`--center-freq 434.4M` / `--channel-freq 434.425M` to tell the server the
absolute frequency (for the network explorer). Without them the client takes
it from the file name: the server's own captures carry the tuner centre as
`_c<Hz>Hz`, and a name like `s6_f434.425_DMR.blue` gives the channel in MHz.
Verified end to end against both server backends with a real DMR
signal wrapped as CI and big-endian CF BLUE files — the WAV comes out
sample-exact in both cases.

`tools/stream_load_test.py` builds on the client to answer capacity
questions: it drives N concurrent realtime streams at a server binary
and reports the measured CPU cost per stream-second (process-tree
accounting, so dsd-fme children count), peak RSS, and a streams-per-box
estimate for the machine it ran on. Run it on the actual deployment
hardware — measured reference points from this repo's verification
environment (2.1 GHz Xeon core, 32 ksps DMR streams, all server
overhead included): ~35 CPU-ms per stream-second voice-active / ~21
idle on the DSDcc backend, ~53 voice-active / ~21 idle on the dsd-fme
backend; ~1.5 MB RSS per DSDcc session vs ~6 MB per dsd-fme session.

`tools/dmr_slot_aggregator.hpp` is a **reference** (not used by the
server): a header-only, std-only C++17 helper a client can use to group
DMR events into per-slot call sessions. DMR is the only 2-slot TDMA
protocol here, so gate it with `is_timeslotted_protocol()`. You feed it
the flat event fields you already parse (no networking/JSON inside); it
handles the concurrent slots, attributes slot-less alias/SMS lines by
source, and closes calls on a terminator or a hang-time timeout. See
`tests/test_dmr_slot_aggregator.cpp` for a worked scenario.

## Protocol

**Full reference: [PROTOCOL.md](PROTOCOL.md)** — every JSON frame the
server can send with exact field-by-field shapes, real captured
examples from both backends, the complete list of error messages, and
the binary audio formats. The tables below are the quick summary.

**Client → Server**

| Frame | Payload | Purpose |
|---|---|---|
| text | `{"type":"start","sample_rate":2000000,"channel_bandwidth":12500,"freq_offset":0,"gain":26000,"afc":false,"protocol":"dmr"}` | Start the pipeline for this connection. `sample_rate` is your IQ rate in Hz. `freq_offset` (positive = channel sits above 0 Hz in your IQ) shifts the channel to baseband. `afc:true` enables automatic frequency control — the server then corrects residual ppm error/drift itself (locks up to ~4 kHz of error in about a second; see PROTOCOL.md). `gain` scales discriminator output into PCM range — see Tuning below. `protocol` (optional) picks the signal chain: absent/`dmr`/`nxdn48`/`p25`/`ysf`/`provoice`/`edacs`/`x2tdma`/… run the FM-discriminator + DSD path (EDACS/ProVoice and X2-TDMA are dsd-fme-backend only); **`tetra` or `tetrakit` switch the whole session to the π/4-DQPSK + TETRA chain** (osmo `tetra-rx` or tetra-kit's `decoder` respectively). The FM-only knobs (`channel_bandwidth`, `set_gain`, `set_freq_offset`) are accepted and ignored under TETRA. |
| text | `{"type":"set_gain","gain":30000}` | Adjust discriminator gain live. |
| text | `{"type":"set_freq_offset","hz":1500}` | Adjust the NCO shift live. |
| text | `{"type":"stop"}` | Stop the pipeline (connection stays open). |
| binary | interleaved little-endian `float32` I/Q samples | Raw IQ block. Ignored until a `start` message has been sent. |

**Server → Client**

| Frame | Payload | Purpose |
|---|---|---|
| text | `{"type":"capabilities","protocols":"...","event_kinds":"...","extra_keys_dmr":"...", ...}` | Sent once on connect, before anything else. Advertises which protocols this build decodes, the event kinds it emits, and the `extra` token keys it can produce (grouped per protocol family) so a client can discover them programmatically. Skip it if you read the first frame expecting `started`. See PROTOCOL.md. |
| text | `{"type":"started","udp_audio_port":47213}` | Pipeline is up. |
| text | `{"type":"event","kind":"call","talkgroup":"19535","source_id":"2222223","slot":"2","extra":"","raw":"..."}` | Decoder activity, parsed from a dsd-fme log line (or synthesized from DSDcc state). `kind` is `voice`/`sync`/`call`/`message`/`unknown` (plus `burst` on DSDcc); `message` carries decoded DMR short-data/SMS text in the `message` field. All fields always present, `""` when unknown. See PROTOCOL.md for per-backend semantics and real examples. |
| text | `{"type":"error","message":"..."}` | Something was rejected (bad control message, invalid key, DSD/TETRA backend failed to start, malformed binary frame). Connection stays open. PROTOCOL.md lists all six message texts. |
| binary | `0x01` + `int16` LE PCM | Decoded voice audio. **8000 Hz mono** from both backends: the dsd-fme backend collapses its stereo (slot1 left / slot2 right) to mono, auto-following the active TDMA slot (downmix when the slot isn't yet known); the DSDcc backend is mono per burst and likewise follows one slot — see PROTOCOL.md. |

## Experimental: RRC symbol matched filter (`matched_filter`)

An **opt-in** root-raised-cosine matched filter can be applied to the
discriminator output before the decoder, matched to the digital-voice
symbol pulse (default DMR: 4800 Bd, 0.2 rolloff). FM discriminator noise
has a rising (parabolic) spectrum, so most of it sits *above* the symbol
band; narrowing the post-detection bandwidth to the symbol band recovers
SNR into the decoder's slicer near threshold. Symbol timing is left to the
decoder (dsd-fme/DSDcc do their own), so this only conditions the stream —
the 48 kHz output rate is unchanged.

Enable it per session with `"matched_filter": true` on the `start` message
(or `--matched-filter` in `tools/midas_ws_client.py`). Default **off**: the
pipeline is byte-identical when unused.

> **Status: experimental, opt-in — not enabled by default.** Lives on the
> `claude/iq-matched-filter` branch. Pure-DSP unit tests
> (`tests/test_matched_filter.cpp`) verify the filter is a correct RRC
> (symmetric, unity DC gain, −3 dB at Rs/2, ISI-free cascade) and that on a
> constant tone + AWGN it preserves the signal level exactly while cutting
> raw-discriminator noise variance by ~14 dB — an *upper bound* on the
> benefit, since the decoder already filters some of that out-of-band noise.

### Measured effect (one real DMR capture + synthetic AWGN)

A real off-air DMR capture (unencrypted voice, TG 1 / SRC 123, complex
float32 @ 20.3 kHz, ~3 s) was decoded with the filter off vs on. With no
added noise it made **no difference on dsd-fme** (it decodes cleanly either
way; dsd-fme already filters internally) and recovered ~13% more voice on
the weaker-front-end DSDcc backend.

Adding AWGN to probe the near-threshold regime — **lock rate = fraction of
5 independent noise seeds that recovered the talkgroup**:

| Added SNR | dsd-fme off | dsd-fme on | DSDcc off | DSDcc on |
|---|---|---|---|---|
| 10 dB | 5/5 | 5/5 | **2/5** | **5/5** |
|  8 dB | 5/5 | 5/5 | **1/5** | **4/5** |
|  6 dB | 5/5 | 5/5 | **1/5** | **2/5** |
|  4 dB | 5/5 | 5/5 | 0/5 | 0/5 |
|  3 dB | **2/5** | **5/5** | — | — |
|  2 dB | **1/5** | **5/5** | — | — |

Takeaways, matching theory (no gain above threshold, a few dB near it):

- **dsd-fme** is the more sensitive decoder (threshold ~2–3 dB here). The
  filter is a no-op in normal conditions but takes near-threshold lock from
  1–2/5 → 5/5 — roughly **1–2 dB of extra margin**, even though it's
  redundant with dsd-fme's internal filtering above threshold.
- **DSDcc** has a weaker front end (threshold ~8–10 dB), and the filter buys
  it a wider **~2–4 dB** of margin.

**Caveats:** one 3-second capture; synthetic AWGN referenced to the
capture's mean power (not a calibrated Eb/N0); low IQ rate (~4.2
samples/symbol, so the RRC is coarse); binary lock as a proxy for BER. Real
weak-signal impairments (multipath, adjacent-channel) aren't modeled.
Reproduce with `tools/midas_ws_client.py … --matched-filter` against a
noise-injected capture; before defaulting it on, validate on more real weak
captures (ideally with a per-frame BER metric instead of lock rate).

## Tuning notes

- **`gain` / `disc_gain`**: maps discriminator output (radians/sample) to
  the 16-bit PCM range DSD-FME expects. There's no universally-correct
  value — it depends on your SDR's actual carrier deviation and
  DSD-FME's internal AGC. Start around the default (26000), watch
  DSD-FME's sync/error stats, and adjust.
- **Sample rate mismatches** are the most common source of "dsd-fme locks
  sync but the decode is garbage." The demod always outputs exactly
  48000 Hz regardless of your input IQ rate (`sample_rate` in the `start`
  message can be anything), so this is mainly a concern in
  `DsdProcessConfig` if you change `mode_flag`/`extra_args` in a way that
  assumes a different input rate.
- **Resampling quality**: the decimated→48kHz step uses linear
  interpolation, which is adequate for voice-grade discriminator audio
  but not as clean as a polyphase resampler. If you're seeing marginal
  sync on weak signals, that's a reasonable place to improve fidelity.
- **`channel_bandwidth`**: defaults to 12500 Hz (standard DMR channel
  spacing). The channel filter's cutoff is derived from this — narrow it
  if you have a strong adjacent-channel interferer.
- **FIR tap count** (`FmDemodConfig::fir_taps`, default 63): more taps
  give sharper channel selectivity at the cost of more CPU per sample and
  more group delay. 63 is a reasonable starting point for a 12.5 kHz DMR
  channel at typical SDR sample rates (1–2.4 MHz).

## Example client (Python)

Minimal example that connects, starts a session, streams a WAV/raw IQ
file as float32 frames, and prints whatever comes back. Needs
`pip install websockets numpy`.

```python
import asyncio
import json
import struct
import numpy as np
import websockets

async def main():
    async with websockets.connect("ws://localhost:22600") as ws:
        await ws.send(json.dumps({
            "type": "start",
            "sample_rate": 2_000_000,
            "channel_bandwidth": 12500,
            "freq_offset": 0,
            "gain": 26000,
        }))

        async def receiver():
            async for msg in ws:
                if isinstance(msg, str):
                    print("EVENT:", msg)
                else:
                    tag, pcm = msg[0], msg[1:]
                    if tag == 0x01:
                        print(f"AUDIO: {len(pcm)//2} samples")

        recv_task = asyncio.create_task(receiver())

        # Replace with your real IQ source; this sends silence as a smoke test.
        block = np.zeros(4096, dtype=np.complex64)
        for _ in range(50):
            await ws.send(block.tobytes())
            await asyncio.sleep(4096 / 2_000_000)

        await ws.send(json.dumps({"type": "stop"}))
        recv_task.cancel()

asyncio.run(main())
```

