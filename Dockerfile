# Dockerfile for dsd-server — Debian bookworm, multi-stage.
#
# Produces a runtime image containing BOTH server variants plus the real
# dsd-fme, so either backend can be run from the same image:
#
#   dsd-server        subprocess backend (spawns the bundled dsd-fme per
#                     session; the image's default command)
#   dsd-server-dsdcc  in-process DSDcc backend (no subprocess)
#
# The three DSP dependencies that Debian doesn't package — mbelib, DSDcc,
# and dsd-fme — are built from source, pinned to the exact commits this
# project's backends were verified against (see the verification notes in
# src/dsd_process.cpp and src/dsdcc_decoder.cpp; bump the pins only in
# step with re-running the real-binary tests). liquid-dsp is deliberately
# not included: the hand-rolled demod outperformed it on x86 in this
# project's own benchmark (see README's liquid section), so the variant
# would only add image weight.
#
# Build:            docker build -t dsd-server .
# Build + run the
# full test suite:  docker build --target test -t dsd-server-test .
#                   (the test stage runs ctest against the real dsd-fme
#                   and a real DMR capture; the build FAILS if any test
#                   fails, so this doubles as CI)
# Run:              docker run --rm -p 8765:8765 dsd-server
#                   docker run --rm -p 8765:8765 dsd-server dsd-server-dsdcc 0.0.0.0 8765 4
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
        libboost-dev \
        libboost-system-dev \
        libsndfile1-dev \
        libncurses-dev \
        libpulse-dev \
    && rm -rf /var/lib/apt/lists/*

# mbelib — AMBE vocoder, needed by both DSDcc and dsd-fme.
ARG MBELIB_COMMIT=9a04ed5c78176a9965f3d43f7aa1b1f5330e771f
RUN git clone https://github.com/szechyjs/mbelib /opt/src/mbelib \
    && git -C /opt/src/mbelib checkout ${MBELIB_COMMIT} \
    && cmake -S /opt/src/mbelib -B /opt/src/mbelib/build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build /opt/src/mbelib/build -j"$(nproc)" \
    && cmake --install /opt/src/mbelib/build \
    && ldconfig

# DSDcc — the in-process decoder backend. Its source tree also carries
# the real DMR capture (samples/dmr_it_8.dis) that arms the test stage.
ARG DSDCC_COMMIT=f27b32d2df131ae3a376fe72d3fb880ae1f9ede1
RUN git clone https://github.com/f4exb/dsdcc /opt/src/dsdcc \
    && git -C /opt/src/dsdcc checkout ${DSDCC_COMMIT} \
    && cmake -S /opt/src/dsdcc -B /opt/src/dsdcc/build -DCMAKE_BUILD_TYPE=Release -DUSE_MBELIB=ON \
    && cmake --build /opt/src/dsdcc/build -j"$(nproc)" \
    && cmake --install /opt/src/dsdcc/build \
    && ldconfig

# dsd-fme — the subprocess backend's decoder binary.
ARG DSDFME_COMMIT=198f0eacb5ef3873fab23186640c90789152894c
RUN git clone https://github.com/lwvmobile/dsd-fme /opt/src/dsd-fme \
    && git -C /opt/src/dsd-fme checkout ${DSDFME_COMMIT} \
    && cmake -S /opt/src/dsd-fme -B /opt/src/dsd-fme/build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build /opt/src/dsd-fme/build -j"$(nproc)" \
    && cmake --install /opt/src/dsd-fme/build \
    && ldconfig

# The project itself. (.dockerignore keeps host build/ and .git out of
# the context, so this is source-only.)
COPY CMakeLists.txt /opt/dsd-server/
COPY src /opt/dsd-server/src
COPY tests /opt/dsd-server/tests
RUN cmake -S /opt/dsd-server -B /opt/dsd-server/build \
        -DCMAKE_BUILD_TYPE=Release \
        -DDSD_FME_BIN=/usr/local/bin/dsd-fme \
        -DDSDCC_SAMPLES_DIR=/opt/src/dsdcc/samples \
    && cmake --build /opt/dsd-server/build -j"$(nproc)" --target dsd-server dsd-server-dsdcc

# ----------------------------------------------------------------- test
# Optional gate: `docker build --target test .` builds every test binary
# and runs the full ctest suite inside the image — including the
# real-capture tests against the real dsd-fme and DSDcc just built
# above. Not part of the default build path, so plain `docker build .`
# stays fast.
FROM build AS test
RUN cmake --build /opt/dsd-server/build -j"$(nproc)" --target \
        test-fake-dsd-fme test_session test_session_concurrency \
        test_dsdcc_decoder test_session_dsdcc \
        test_dsd_process test_session_real_fme \
        test_fm_demod test_afc \
    && cd /opt/dsd-server/build && ctest --output-on-failure

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
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --user-group --no-create-home dsd

COPY --from=build /usr/local/lib/libmbe.so* /usr/local/lib/
COPY --from=build /usr/local/lib/libdsdcc.so* /usr/local/lib/
COPY --from=build /usr/local/bin/dsd-fme /usr/local/bin/
COPY --from=build /opt/dsd-server/build/dsd-server /usr/local/bin/
COPY --from=build /opt/dsd-server/build/dsd-server-dsdcc /usr/local/bin/
RUN ldconfig

USER dsd
EXPOSE 8765

# args after the image name replace CMD: address, port, io threads —
# or name a different binary entirely (dsd-server-dsdcc ...).
ENTRYPOINT ["tini", "--"]
CMD ["dsd-server", "0.0.0.0", "8765", "4"]

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
#     -> 8 realtime streams against the DSDcc backend, prints measured
#        CPU-ms per stream-second and a streams-per-box estimate for
#        the machine the container is running on
#
# Arguments replace the CMD (server binary, BLUE file, stream count):
#
#   docker run --rm dsd-server-loadtest \
#       /usr/local/bin/dsd-server /opt/dsd-server/testdata/dmr_32k.tmp 8
#   docker run --rm -v $PWD/mycapture.tmp:/data/c.tmp dsd-server-loadtest \
#       /usr/local/bin/dsd-server-dsdcc /data/c.tmp 16
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
COPY --from=build /opt/src/dsdcc/samples/dmr_it_8.dis /opt/dsd-server/testdata/
RUN python3 /opt/dsd-server/tools/make_test_bluefile.py \
        /opt/dsd-server/testdata/dmr_it_8.dis \
        /opt/dsd-server/testdata/dmr_32k.tmp --rate 32000
USER dsd
ENTRYPOINT ["tini", "--", "python3", "/opt/dsd-server/tools/stream_load_test.py"]
CMD ["/usr/local/bin/dsd-server-dsdcc", "/opt/dsd-server/testdata/dmr_32k.tmp", "8"]
