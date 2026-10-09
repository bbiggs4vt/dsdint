# Dockerfile for dsd-server — Debian bookworm, multi-stage.
#
# Produces a runtime image containing the server variants plus the real
# decoders they spawn, so any backend runs from the same image:
#
#   dsd-server          spawns the bundled dsd-fme per session; the image's
#                       default command
#
# TETRA is NOT a separate executable: every variant decodes it at run time from
# the client's "protocol" hint — "tetra" (via the bundled osmo tetra-rx) or
# "tetrakit" (via the bundled tetra-kit decoder). Both decoders are on the
# image's PATH.
#
# TETRA voice: TETRA sessions decode events and extract speech frames. By
# default the image ships NO ACELP codec (patent-encumbered / GPLv3 — see
# TETRA_VOICE.md), so they emit events only, not decoded audio. Opt in with
# `--build-arg TETRA_CODEC=1` to compile the codec (from the already-cloned
# tetra-kit) into the servers so TETRA sessions emit audio; such an image links
# GPLv3 code (see the TETRA_CODEC arg below before redistributing).
#
# Paging is likewise a runtime hint, not a separate executable: "pager-auto",
# "pocsag*" or "flex" runs an FM -> multimon-ng chain (the bundled multimon-ng,
# built from upstream source because Debian's 1.3.0 package lacks the --json
# output and FLEX_NEXT decoder the server needs).
#
# The DSP dependencies that Debian doesn't package — mbelib, dsd-fme,
# osmo tetra-rx, tetra-kit and multimon-ng — are built from source, pinned to
# exact commits (dsd-fme to the commit this project's backend was verified
# against — see the notes in src/dsd_process.cpp; bump that pin only in step
# with re-running the real-binary tests). dsd-fme gets one small patch,
# patches/dsd-fme-keep-bp-key.patch (see the dsd-fme step).
#
# Build:            docker build -t dsd-server .
# Build + run the
# full test suite:  docker build --target test -t dsd-server-test .
#                   (the test stage runs ctest against the real dsd-fme
#                   and a real DMR capture; the build FAILS if any test
#                   fails, so this doubles as CI)
# Run:              docker run --rm -p 22600:22600 dsd-server
#                   docker run --rm -p 22600:22600 dsd-server dsd-server 0.0.0.0 22600 4
# Capacity test:    docker build --target loadtest -t dsd-server-loadtest .
#                   docker run --rm dsd-server-loadtest
#                   (measures N concurrent realtime streams ON THIS
#                   MACHINE and prints CPU per stream + streams-per-box;
#                   see the loadtest stage below for arguments)
#
# Boost note: bookworm ships Boost 1.74, which is why the project's
# CMakeLists floor is 1.74. If you rebase this image onto trixie or
# newer, nothing here needs to change.

# ---------------------------------------------------------------- build
FROM debian:bookworm AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        git \
        ca-certificates \
        pkg-config \
        libboost-dev \
        libboost-system-dev \
        libsndfile1-dev \
        libncurses-dev \
        libpulse-dev \
        zlib1g-dev \
        libosmocore-dev \
        rapidjson-dev \
    && rm -rf /var/lib/apt/lists/*

# mbelib — AMBE vocoder, needed by dsd-fme.
ARG MBELIB_COMMIT=9a04ed5c78176a9965f3d43f7aa1b1f5330e771f
RUN git clone https://github.com/szechyjs/mbelib /opt/src/mbelib \
    && git -C /opt/src/mbelib checkout ${MBELIB_COMMIT} \
    && cmake -S /opt/src/mbelib -B /opt/src/mbelib/build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build /opt/src/mbelib/build -j"$(nproc)" \
    && cmake --install /opt/src/mbelib/build \
    && ldconfig

# The real DMR capture (samples/dmr_it_8.dis) that arms the real-fme test
# stage ships in the DSDcc source tree. Shallow-clone it for that one sample
# only -- the DSDcc decoder backend itself is no longer built.
RUN git clone --depth 1 https://github.com/f4exb/dsdcc /opt/src/dsd-samples

# dsd-fme — the decoder binary the server spawns per session. Patched so a
# DMR Basic Privacy key given with -b survives carrier loss while a key list
# (-K) is loaded -- unpatched, dsd-fme zeroes it at every carrier drop, so the
# explorer's BP keys stopped working after the first call on any DMR stream
# that also had keyring / EP keys. The patch is re-checked against the pin.
ARG DSDFME_COMMIT=198f0eacb5ef3873fab23186640c90789152894c
COPY patches/dsd-fme-keep-bp-key.patch /opt/src/
RUN git clone https://github.com/lwvmobile/dsd-fme /opt/src/dsd-fme \
    && git -C /opt/src/dsd-fme checkout ${DSDFME_COMMIT} \
    && git -C /opt/src/dsd-fme apply /opt/src/dsd-fme-keep-bp-key.patch \
    && cmake -S /opt/src/dsd-fme -B /opt/src/dsd-fme/build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build /opt/src/dsd-fme/build -j"$(nproc)" \
    && cmake --install /opt/src/dsd-fme/build \
    && ldconfig

# tetra-rx — the osmo (sq5bpf fork) TETRA decoder the server spawns for a
# protocol":"tetra" session. Reads the demodulated bitstream on stdin, emits
# TETMON events over UDP. Links libosmocore (installed above).
ARG OSMO_TETRA_COMMIT=4e9f1b23b460a6b24270ece685ac68d4d7fd4cc8
RUN git clone https://github.com/sq5bpf/osmo-tetra-sq5bpf /opt/src/osmo-tetra \
    && git -C /opt/src/osmo-tetra checkout ${OSMO_TETRA_COMMIT} \
    && make -C /opt/src/osmo-tetra/src tetra-rx \
    && install -m 0755 /opt/src/osmo-tetra/src/tetra-rx /usr/local/bin/tetra-rx

# tetra-kit decoder — the second TETRA decoder, spawned for a
# protocol":"tetrakit" session. Reads bits over UDP, emits JSON reports over
# UDP. Needs rapidjson (header-only) + zlib.
ARG TETRA_KIT_COMMIT=7306a08c2dc753de636aaea8187e6ac5a3a99d02
RUN git clone https://gitlab.com/larryth/tetra-kit /opt/src/tetra-kit \
    && git -C /opt/src/tetra-kit checkout ${TETRA_KIT_COMMIT} \
    && make -C /opt/src/tetra-kit/decoder \
    && install -m 0755 /opt/src/tetra-kit/decoder/decoder /usr/local/bin/decoder

# multimon-ng — the paging decoder (POCSAG 512/1200/2400 + FLEX_NEXT), spawned
# per paging session with --json. Built headless (no X11 / PulseAudio / SDL
# scope) so the binary needs nothing beyond libc/libm at run time. Its
# gen-ng and bundled off-air POCSAG/FLEX samples feed the test stage.
ARG MULTIMON_NG_COMMIT=0722194b7739748e49f18ac1fc76f236d4ca390d
RUN git clone https://github.com/EliasOenal/multimon-ng /opt/src/multimon-ng \
    && git -C /opt/src/multimon-ng checkout ${MULTIMON_NG_COMMIT} \
    && cmake -S /opt/src/multimon-ng -B /opt/src/multimon-ng/build -DCMAKE_BUILD_TYPE=Release \
        -DX11_SUPPORT=OFF -DPULSE_AUDIO_SUPPORT=OFF -DSDL3_SCOPE=OFF \
    && cmake --build /opt/src/multimon-ng/build -j"$(nproc)" \
    && install -m 0755 /opt/src/multimon-ng/build/multimon-ng /usr/local/bin/multimon-ng

# The project itself. (.dockerignore keeps host build/ and .git out of
# the context, so this is source-only.)
COPY CMakeLists.txt /opt/dsd-server/
COPY src /opt/dsd-server/src
COPY tests /opt/dsd-server/tests
COPY tools /opt/dsd-server/tools
# Real off-air paging captures (X-Midas BLUE) used by test_session_pager.
COPY samples /opt/dsd-server/samples
# The explorer's user manual, built into the server (its Help button).
COPY docs/NET_EXPLORER.pdf /opt/dsd-server/docs/NET_EXPLORER.pdf
# Optional TETRA voice codec. TETRA speech is ACELP (ETSI EN 300 395-2); the
# reference codec is patent-encumbered and, as carried by tetra-kit, GPLv3, so
# it is NOT vendored in the repo and OFF by default (image stays events-only).
# `--build-arg TETRA_CODEC=1` compiles tetra-kit's already-cloned recorder/audio
# codec into the servers so TETRA sessions emit decoded audio -- for your own
# locally-built image. Such an image links GPLv3 code; consider that before
# redistributing it. (No extra download: tetra-kit is already cloned above.)
ARG TETRA_CODEC=0
RUN CODEC_ARGS=""; \
    if [ "$TETRA_CODEC" = "1" ]; then \
        CODEC_ARGS="-DDSD_WITH_TETRA_CODEC=ON -DTETRA_KIT_DIR=/opt/src/tetra-kit"; \
        echo "== Building WITH the TETRA ACELP voice codec (GPLv3) =="; \
    fi; \
    cmake -S /opt/dsd-server -B /opt/dsd-server/build \
        -DCMAKE_BUILD_TYPE=Release \
        -DDSD_FME_BIN=/usr/local/bin/dsd-fme \
        -DDSD_SAMPLES_DIR=/opt/src/dsd-samples/samples \
        -DMULTIMON_NG_BIN=/usr/local/bin/multimon-ng \
        -DPAGER_FIXTURES_DIR=/opt/dsd-server/build/pager_fixtures \
        $CODEC_ARGS \
    && cmake --build /opt/dsd-server/build -j"$(nproc)" --target \
        dsd-server

# ----------------------------------------------------------------- test
# Optional gate: `docker build --target test .` builds every test binary
# and runs the full ctest suite inside the image — including the
# real-capture tests against the real dsd-fme built above. Not part of the
# default build path, so plain `docker build .` stays fast.
#
# On slow hardware, slow the full-stack tests' IQ feed down to match:
# the two session tests stream the capture at ~8.5x realtime by default
# (85 ms of IQ every DSD_TEST_PACE_MS=10 ms), which assumes the machine
# decodes that much faster than realtime. A box that can't keep up
# (e.g. a 1.2 GHz ARMv7) overflows the session's 64-block IQ queue —
# which drops oldest by design — and the tests fail with truncated
# decoded audio despite the pipeline being healthy at realtime. Raise
# the pace toward realtime there:
#
#   docker build --target test --build-arg DSD_TEST_PACE_MS=60 .
#
# (60 ms/block is ~1.4x realtime; the QEMU-emulated ARM64 runs use 100.)
FROM build AS test
ARG DSD_TEST_PACE_MS=10
# Same opt-in as the build stage: with TETRA_CODEC=1 the codec round-trip test
# is built and run too (it is only registered in a codec build).
ARG TETRA_CODEC=0
# sox converts multimon-ng's bundled FLAC captures into the paging fixtures.
RUN apt-get update && apt-get install -y --no-install-recommends sox \
    && rm -rf /var/lib/apt/lists/* \
    && /opt/dsd-server/tools/make_pager_fixtures.sh /opt/src/multimon-ng /opt/dsd-server/build/pager_fixtures
RUN cmake --build /opt/dsd-server/build -j"$(nproc)" --target \
        test-fake-dsd-fme test_session test_session_concurrency \
        test_dsd_process test_session_real_fme \
        test_dsd_fme_parse \
        test_fake_dsd_server fake_dsd_server \
        test_tetra_demod test_tetra_burst_sync test_tetra_frontend \
        tetra_bit_source test_tetra_bit_source \
        test_tetmon_parse test_tetra_process tetra_fake_rx \
        test_tetra_kit_json test_tetra_kit_process tetra_kit_fake \
        test_tetra_voice \
        test_fm_demod test_matched_filter test_dmr_slot_aggregator test_afc \
        test_server_stats \
        test_pager_demod test_pager_events test_session_pager \
    && if [ "$TETRA_CODEC" = "1" ]; then \
           cmake --build /opt/dsd-server/build -j"$(nproc)" --target test_tetra_codec; \
       fi \
    && cd /opt/dsd-server/build \
    && DSD_TEST_PACE_MS=${DSD_TEST_PACE_MS} ctest --output-on-failure

# -------------------------------------------------------------- runtime
FROM debian:bookworm-slim AS runtime

# tini: dsd-server installs no signal handlers, and a bare PID 1 ignores
# SIGTERM by default — without an init, `docker stop` would hang for the
# grace period and then SIGKILL. tini also reaps any dsd-fme child a
# crashed session leaves behind.
RUN apt-get update && apt-get install -y --no-install-recommends \
        tini \
        libboost-system1.74.0 \
        libsndfile1 \
        libncursesw6 \
        libpulse0 \
        zlib1g \
        libtalloc2 \
        libsctp1 \
        libmnl0 \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --user-group --no-create-home dsd

COPY --from=build /usr/local/lib/libmbe.so* /usr/local/lib/
COPY --from=build /usr/local/bin/dsd-fme /usr/local/bin/
# TETRA: the two external decoders and libosmocore (tetra-rx's only non-system
# dependency; libtalloc/libsctp/libmnl come from apt above). The soname is
# whatever the build stage's libosmocore-dev provided, copied verbatim so it
# always matches the tetra-rx built against it.
COPY --from=build /usr/lib/x86_64-linux-gnu/libosmocore.so.* /usr/lib/x86_64-linux-gnu/
COPY --from=build /usr/local/bin/tetra-rx /usr/local/bin/
COPY --from=build /usr/local/bin/decoder /usr/local/bin/
# Paging decoder (headless build: libc/libm only). The server runs it under
# coreutils' stdbuf (in the base image) to line-buffer its JSON output.
COPY --from=build /usr/local/bin/multimon-ng /usr/local/bin/
COPY --from=build /opt/dsd-server/build/dsd-server /usr/local/bin/
RUN ldconfig

# Make IQ capture work out of the box. The runtime sets no WORKDIR, so the
# container's cwd is / -- which the non-root `dsd` user can't write to, so the
# default capture dir (".") would silently fail to open a file. Ship a writable
# capture dir owned by dsd and point DSD_IQ_LOG_DIR at it, so enabling capture
# (the "Log IQ" switch, or a client's iq_log:true) just works. Declared a VOLUME
# so the files are easy to retrieve: bind-mount it, e.g.
#   docker run -v "$PWD/caps:/captures" -p 22600:22600 dsd-server
# (DSD_IQ_LOG_MAX_MB still caps each file; see README.)
RUN mkdir -p /captures && chown dsd:dsd /captures
ENV DSD_IQ_LOG_DIR=/captures
VOLUME ["/captures"]

USER dsd
EXPOSE 22600

# args after the image name replace CMD: address, port, io threads —
# (e.g. a different address/port).
ENTRYPOINT ["tini", "--"]
CMD ["dsd-server", "0.0.0.0", "22600", "4"]

# ------------------------------------------------------------- loadtest
# Self-contained capacity measurement: runs tools/stream_load_test.py
# INSIDE the container (the tool spawns the server itself and measures
# its process tree, so it has to share the machine and PID view with
# it). Ships a ready-made 20 s test capture -- the repo's verified real
# DMR signal FM-wrapped as 32 ksps IQ -- so the zero-argument form just
# works:
#
#   docker build --target loadtest -t dsd-server-loadtest .
#   docker run --rm dsd-server-loadtest
#     -> 8 realtime streams, prints measured
#        CPU-ms per stream-second and a streams-per-box estimate for
#        the machine the container is running on
#
# Arguments replace the CMD (server binary, BLUE file, stream count):
#
#   docker run --rm dsd-server-loadtest \
#       /usr/local/bin/dsd-server /opt/dsd-server/testdata/dmr_32k.tmp 8
#   docker run --rm -v $PWD/mycapture.tmp:/data/c.tmp dsd-server-loadtest \
#       /usr/local/bin/dsd-server /data/c.tmp 16
#
# To measure at a different IQ sample rate, pass the baked-in raw
# discriminator capture plus --rate; test IQ at that rate is generated
# inside the container before the run:
#
#   docker run --rm dsd-server-loadtest /usr/local/bin/dsd-server \
#       /opt/dsd-server/testdata/dmr_it_8.dis 8 --rate 64000
#
# Run it on the DEPLOYMENT machine -- the numbers describe wherever the
# container executes. Give the container all cores (no --cpus limit) for
# a whole-box answer, or set --cpus to measure a deliberate budget.
FROM runtime AS loadtest
USER root
RUN apt-get update && apt-get install -y --no-install-recommends \
        python3 \
    && rm -rf /var/lib/apt/lists/*
COPY tools/ /opt/dsd-server/tools/
COPY --from=build /opt/src/dsd-samples/samples/dmr_it_8.dis /opt/dsd-server/testdata/
RUN python3 /opt/dsd-server/tools/make_test_bluefile.py \
        /opt/dsd-server/testdata/dmr_it_8.dis \
        /opt/dsd-server/testdata/dmr_32k.tmp --rate 32000
USER dsd
ENTRYPOINT ["tini", "--", "python3", "/opt/dsd-server/tools/stream_load_test.py"]
CMD ["/usr/local/bin/dsd-server", "/opt/dsd-server/testdata/dmr_32k.tmp", "8"]

# -------------------------------------------------------------- default
# Docker builds the LAST stage when no --target is given, and loadtest
# has to sit after runtime (it derives FROM it) — so without this, a
# plain `docker build .` would produce the load-test image instead of
# the server. This empty re-selection makes the default build the
# runtime image, as every usage example above assumes.
FROM runtime
