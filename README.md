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

## What's implemented

- `src/fm_demod.{hpp,cpp}` — the original hand-rolled streaming FM
  discriminator: NCO frequency shift (optional), windowed-sinc FIR
  channel filter, integer decimation, quadrature demod (`atan2` of
  `x[n] * conj(x[n-1])`), linear resample to exactly 48000 Hz, gain
  scaling to 16-bit PCM.
- `src/fm_demod_liquid.{hpp,cpp}` — a liquid-dsp-based alternative with
  the *same* public interface, for A/B performance testing on your
  target hardware. Uses `nco_crcf` for the optional frequency shift,
  a single `msresamp_crcf` (polyphase arbitrary-rate resampler) that
  does channel filtering *and* rate conversion in one step, and
  `freqdem` for the discriminator. See "The liquid-dsp variant" below
  before using this one.
- `src/fm_demod_selector.hpp` — compile-time switch between the two
  (`-DDSD_USE_LIQUID_DEMOD`), so `session.cpp` doesn't need to know or
  care which backend it's linked against. This is what makes the A/B
  comparison apples-to-apples: identical networking code, identical
  dsd-fme plumbing, only the DSP core differs between the two binaries.
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
  three backend swaps in this project so far — see "The DSDcc variant"
  below, and read the top-of-file comment in `dsdcc_decoder.cpp` before
  trusting any of it.
- `src/dsd_backend_selector.hpp` — compile-time switch between the two
  DSD backends (`-DDSD_USE_DSDCC_BACKEND`), mirroring
  `fm_demod_selector.hpp`'s role for the demod backends.
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
  FM-modulated tone (see `src/test_fm_demod.cpp`). Confirms the streaming
  block/history-carry logic doesn't crash across chunk boundaries and
  that the recovered discriminator magnitude lands in the theoretically
  expected range for a known injected deviation. This is a sanity check,
  **not** validation against a real DMR capture — you should verify
  against your own SDR before trusting it operationally.
- **`dsd_process.cpp`** — compiled and run end-to-end against a fake
  `dsd-fme` stand-in script that echoes its argv, logs synthetic event
  lines, and sends fake UDP audio. Confirmed stdin PCM delivery, stdout
  line parsing, and UDP audio receipt all work correctly through the real
  fork/exec/pipe/socket code path.
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
  When `fm_demod_selector.hpp`/`ActiveFmDemodulator` were introduced to
  support the liquid-dsp variant, the only changes to these two files
  were mechanical (swap one `#include`, swap the demod type name in two
  places) — the logic reviewed above is otherwise untouched.
- **`fm_demod_liquid.cpp`** — originally code-reviewed against
  liquid-dsp's public API docs and example code without compiling it (no
  liquid-dsp available in my sandbox). **Since then, you've built and run
  `test_fm_demod_liquid` against real liquid-dsp** and it passed — output
  RMS matched the theoretical value for a correctly-scaled discriminator
  to within 0.2%, confirming the one call I flagged as uncertain
  (`freqdem_demodulate_block`) is correct. See "The liquid-dsp variant"
  below.
- **`dsd_process.cpp`** and **`session.cpp`'s use of it** — these are the
  same files described above; no changes beyond what's already noted.
- **`dsdcc_decoder.cpp`** — this one's meaningfully less verified than
  the liquid variant. I only had fragments of DSDcc's headers to go on
  (no confirmed real-world usage example for the main call), so the
  core sample-ingestion API call is a placeholder, not a checked
  signature. I did syntax-check my own code against a stub header
  matching my assumptions (catches typos/logic errors in my file) —
  that is **not** the same as validating against DSDcc's real API. See
  "The DSDcc variant" below; the file's own top-of-file comment has the
  full confidence breakdown, piece by piece.
- **`test_session.cpp`** — an integration test for `session.cpp` itself
  (real `Server`, real Beast WebSocket client, exercises the actual wire
  protocol). Like `session.cpp` originally was, this is reviewed but not
  compiled by me — see "Testing session.cpp" below.

**Before relying on this**, build it, run it against Boost's compiler
warnings/sanitizers (`-fsanitize=thread` is worth a run given the
multi-threaded design), and test against a real IQ source and a real
`dsd-fme` build.

## Build

Dependencies: a C++17 compiler, CMake ≥ 3.16, Boost ≥ 1.75 (headers +
`boost_system`), and `dsd-fme` built/installed separately
(https://github.com/lwvmobile/dsd-fme). liquid-dsp
(https://github.com/jgaeddert/liquid-dsp) is optional — see below.

```bash
# Debian/Ubuntu
sudo apt install build-essential cmake libboost-dev libboost-system-dev

mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j
./dsd-server            # listens on 0.0.0.0:8765 by default
```

If CMake finds liquid-dsp on your system, it also builds
`dsd-server-liquid` automatically (see below) — no separate flag needed.
If it's not found, you'll see a `liquid-dsp not found` status message
and only `dsd-server` gets built; that's expected, not an error.

To run just the DSP sanity test (no Boost needed):
```bash
make test_fm_demod
./test_fm_demod
```

## The liquid-dsp variant

`fm_demod_liquid.cpp` is a second implementation of the same
`FmDemodulator` interface, built on liquid-dsp
(https://github.com/jgaeddert/liquid-dsp) instead of hand-rolled DSP.
The idea is to A/B test it against the original on your actual target
hardware — the two binaries (`dsd-server` and `dsd-server-liquid`) run
identical networking and `dsd-fme` plumbing, so any performance
difference you measure comes from the demod backend alone.

**Important: I wrote this without being able to compile it.** The
sandbox this was built in has no network access, so liquid-dsp couldn't
be installed and none of `fm_demod_liquid.cpp` has been built or run —
unlike `fm_demod.cpp`, which was compiled and sanity-tested (see below).
The function signatures used (`msresamp_crcf_create`/`_execute`,
`nco_crcf_create`/`_mix_block_down`, `freqdem_create`) are based on
liquid-dsp's published API docs and example code, and I'm reasonably
confident in all of them except one: **`freqdem_demodulate_block`'s
exact name** — liquid-dsp added block-level execution for FM
demodulation in v1.3.0, but I couldn't verify the precise function name
against a real header. If the build fails there, it's almost certainly
just a rename, not a logic problem — the comment block at the top of
`fm_demod_liquid.cpp` explains what to check.

**Before you A/B test with this:**

1. Install liquid-dsp (`libliquid-dev` if your distro packages it, or
   build from source — it's MIT licensed).
2. Build `test_fm_demod_liquid` first and run it — it's a standalone
   sanity check (synthetic FM tone in, check the output magnitude is
   plausible) that doesn't touch Boost, dsd-fme, or the network:
   ```bash
   make test_fm_demod_liquid
   ./test_fm_demod_liquid
   ```
   **Update, confirmed on real hardware:** this has now actually been
   built and run against real liquid-dsp (not just reviewed against
   docs) and it passes — output RMS matched the theoretically expected
   value for a correctly-scaled discriminator to within 0.2%, which
   confirms `freqdem_demodulate_block`'s name/signature (the one call I
   was least sure about) is correct as written. If you see a handful of
   clipped samples right at the start of the run, that's an
   `msresamp_crcf` settling transient, not a bug — the test only warns
   if clipping is sustained across more than 5% of the output.
3. Once that passes, build and run `dsd-server-liquid` the same way as
   `dsd-server`, against the same `dsd-fme` setup, and compare.

**Architectural note**: the liquid version demodulates FM *after*
resampling straight to 48000 Hz (a single `msresamp_crcf` does channel
filtering and rate conversion together), rather than demodulating at a
higher intermediate rate and resampling afterward like the hand-rolled
version does. That's a valid simplification specifically because DMR's
peak deviation (~2 kHz) is well under 48kHz's Nyquist frequency
(24kHz) — see the comments in `fm_demod_liquid.hpp` if you ever repurpose
this for a wider-deviation mode.

## The DSDcc variant

`dsdcc_decoder.cpp` swaps out the *DSD backend* (not the demod) —
instead of spawning a `dsd-fme`/classic-`dsd` subprocess and talking to
it over pipes and a UDP socket, it links DSDcc
(https://github.com/f4exb/dsdcc) directly and pushes samples into a
`DSDDecoder` object in-process. This is a structurally bigger change
than either of the other two variants: no child process, no pipes, no
UDP audio port. If it works out, it's also a more direct answer to
running many concurrent sessions on constrained hardware than the
liquid-dsp swap is — 16 sessions means 16 decoder *objects* in your
existing threads instead of 16 forked OS processes competing for cores.

**This one needs real work before it does anything.** Unlike the
liquid-dsp variant, I didn't have a confirmed usage example to check the
main sample-ingestion call against — only fragments of DSDcc's headers
surfaced via search. Concretely, as shipped:

- `DsdccDecoder::write_audio()` compiles and accepts samples, but the
  actual DSDcc call inside it (`decoder_->run(sample)`) is a low-confidence
  guess at the method name/signature, and the audio/metadata extraction
  around it is placeholder code that will silently produce **zero**
  decoded output — no audio, no events — until you replace it with real
  calls from your installed `dsd_decoder.h`/`dmr.h`. It prints a loud
  warning to stderr on startup for exactly this reason, so it's hard to
  mistake "compiles and runs" for "works."
- The header include paths (`<dsdcc/dsd_decoder.h>`) are also a guess —
  fix the include path in `CMakeLists.txt`/`dsdcc_decoder.cpp` if your
  install exposes them differently.
- `dsdcc_decoder.cpp`'s own top-of-file comment has a full confidence
  breakdown (what's confirmed from header fragments vs. inferred vs.
  placeholder) — read that before making any changes, it tells you
  exactly which lines to fix first.

**What I'd actually trust regardless of the exact API names**: the
overall control flow (push samples → check for decoded audio → check
for state changes → invoke callbacks, all synchronously, no extra
threads needed) matches DSDcc's own documented integration pattern, and
`session.cpp` needed zero changes to handle a backend whose callbacks
fire synchronously on the demod worker thread instead of a dedicated
reader thread — its "always post to the connection's strand before
touching `ws_`" pattern already covered that case generically.

**Getting this running:**

1. Build DSDcc and mbelib (DSDcc needs mbelib for actual voice
   synthesis, same dependency as the other two backends).
2. Open your installed `dsd_decoder.h` and `dmr.h`, find the real
   sample-ingestion method and the metadata/audio getters, and replace
   the placeholders in `dsdcc_decoder.cpp` — start with the LOW
   CONFIDENCE items listed in that file's top comment.
3. `cmake .. && make dsd-server-dsdcc` — CMake finds DSDcc/mbelib the
   same way it finds liquid-dsp for the other variant (skipped with a
   message if not found, not an error).
4. There's no synthetic sanity test for this one, unlike
   `test_fm_demod`/`test_fm_demod_liquid` — generating a valid encoded
   DMR bitstream to feed it is a lot harder than synthesizing an FM
   tone, so testing this needs a real DMR capture or a live signal.

## Comparing the two demod backends: demod_benchmark

`demod_benchmark.cpp` is a head-to-head performance comparison, not a
correctness test — it runs `FmDemodulator` and `FmDemodulatorLiquid`
against identical synthetic input in the same process so the numbers are
directly comparable, no Boost/dsd-fme/network involved.

**Phase 1** measures single-threaded throughput with and without a
frequency offset active. This directly targets the NCO cost difference
discussed earlier in this project's development: the hand-rolled version
calls `std::cos`/`std::sin` per sample at full input rate whenever a
frequency offset is set, while liquid's `nco_crcf` avoids that. If that
reasoning was right, the gap between the two implementations should
shrink a lot with `freq_offset=0` compared to a nonzero offset — worth
checking whether your real numbers bear that out.

**Phase 2** measures concurrency scaling: N independent demod instances
(private data each, no shared state) running simultaneously, sweeping a
fixed `{1,2,4,8,16}` thread count regardless of how many cores the
machine actually has — deliberately covering the oversubscribed case
directly rather than stopping at `hardware_concurrency()`, since that's
the scenario this project has actually been discussing (16 sessions on
an 8-core ARM64 target).

**What this does and doesn't tell you:** it only measures the demod
stage. The DSD decode itself (`dsd-fme`/DSDcc) is a separate, plausibly
larger cost per session at high concurrency — this benchmark answers
"does liquid-dsp help the piece it can help," not "will N sessions fit
on this machine." The benchmark prints that same reminder in its output.

Build and run:
```bash
make demod_benchmark   # builds against liquid too, if CMake found it earlier
./demod_benchmark              # defaults to 2,000,000 Hz input rate
./demod_benchmark 1000000      # or pass your actual IQ rate as argv[1]
```

I compiled and ran this myself in the hand-rolled-only mode (no liquid
available in my sandbox) and it produced exactly the result the NCO
reasoning predicts: with `freq_offset=1500`, the hand-rolled
implementation ran about **2.4x slower** than with `freq_offset=0` on the
same input. I also syntax-checked the liquid-enabled code path against a
stub header matching liquid's real function signatures — same caveat as
everywhere else in this project: that confirms my code is internally
consistent, not that it's correct against the real library. The
concurrency phase ran correctly too, though the sandbox I built this in
only reports one CPU core, so its numbers there aren't informative about
real scaling — that part specifically needs your actual multi-core
target to mean anything.

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
care what audio it receives), the liquid-dsp or DSDcc backends
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
./dsd-server 0.0.0.0 8765 4
```

Make sure `dsd-fme` is on `PATH`, or edit `DsdProcessConfig::dsd_fme_path`
in `session.cpp` to an absolute path.

**Verify the `dsd-fme` CLI flags for your installed version** — they've
changed across releases/forks. `build_argv()` in `dsd_process.cpp` and
the stdout-parsing regexes in `classify_line()` are both marked with
comments pointing at exactly what to check against `dsd-fme -h` and your
own captured log output.

## Protocol

**Client → Server**

| Frame | Payload | Purpose |
|---|---|---|
| text | `{"type":"start","sample_rate":2000000,"channel_bandwidth":12500,"freq_offset":0,"gain":26000}` | Start the demod + dsd-fme pipeline for this connection. `sample_rate` is your IQ rate in Hz. `freq_offset` shifts the channel to baseband if it isn't already centered at 0 Hz in your IQ stream. `gain` scales discriminator output into PCM range — see Tuning below. |
| text | `{"type":"set_gain","gain":30000}` | Adjust discriminator gain live. |
| text | `{"type":"set_freq_offset","hz":1500}` | Adjust the NCO shift live. |
| text | `{"type":"stop"}` | Stop the pipeline (connection stays open). |
| binary | interleaved little-endian `float32` I/Q samples | Raw IQ block. Ignored until a `start` message has been sent. |

**Server → Client**

| Frame | Payload | Purpose |
|---|---|---|
| text | `{"type":"started","udp_audio_port":47213}` | Pipeline is up. |
| text | `{"type":"event","kind":"voice","talkgroup":"12345","source_id":"6789","slot":"1","raw":"..."}` | Parsed from a `dsd-fme` stdout line. `kind` is a best-effort classification (`voice`, `sync`, `call`, `unknown`) — tighten `classify_line()` once you've seen your build's real log format. |
| text | `{"type":"error","message":"..."}` | Something went wrong (bad control message, dsd-fme failed to start, malformed binary frame). |
| binary | `0x01` + `int16` LE PCM | Decoded voice audio from `dsd-fme`'s UDP output. Sample rate depends on your `dsd-fme` build (often 8000 Hz mono) — verify against your version. |

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
    async with websockets.connect("ws://localhost:8765") as ws:
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

## Extending

- **Trunked DMR**: add `-T` / `-C <freq>` (verify flag names) to
  `DsdProcessConfig::extra_args` and expose them through the `start`
  control message.
- **Multiple simultaneous channels per client**: currently one
  `FmDemodulator` + one `dsd-fme` per WebSocket connection. If you want
  one client to monitor several channels at once, the natural extension
  is multiple named "channel" sub-pipelines per `Session`, each tagging
  its outgoing frames with a channel ID.
- **JSON event richness**: `dsd-fme`'s newer builds may expose structured
  (JSON) event output directly — if yours does, swap `classify_line()`'s
  regex parsing for that instead; it'll be far more reliable than
  scraping human-readable log lines.
