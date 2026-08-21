cmake_minimum_required(VERSION 3.16)
project(dsd_server CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")

find_package(Boost 1.75 REQUIRED COMPONENTS system)
find_package(Threads REQUIRED)

add_executable(dsd-server
  src/main.cpp
  src/session.cpp
  src/dsd_process.cpp
  src/fm_demod.cpp
)
target_include_directories(dsd-server PRIVATE src ${Boost_INCLUDE_DIRS})
target_link_libraries(dsd-server PRIVATE Boost::system Threads::Threads)
target_compile_options(dsd-server PRIVATE -Wall -Wextra)

# Standalone sanity test for the DSP core (no Boost/network dependency).
add_executable(test_fm_demod EXCLUDE_FROM_ALL src/fm_demod.cpp src/test_fm_demod.cpp)
target_include_directories(test_fm_demod PRIVATE src)

# Head-to-head performance comparison of the demod backends. Always
# buildable against the hand-rolled implementation alone; liquid-dsp
# support is layered on below (once we know whether liquid was found)
# rather than gating the whole target on it, so this stays available the
# same way test_fm_demod always is.
add_executable(demod_benchmark EXCLUDE_FROM_ALL src/fm_demod.cpp src/demod_benchmark.cpp)
target_include_directories(demod_benchmark PRIVATE src)
target_link_libraries(demod_benchmark PRIVATE Threads::Threads)
target_compile_options(demod_benchmark PRIVATE -Wall -Wextra)

# --- liquid-dsp variant, for A/B performance testing against the above ---
# Built alongside dsd-server whenever liquid-dsp is found; skipped (with a
# message, not an error) otherwise, so this doesn't break the default
# build for anyone who hasn't installed it. See README.md for what's
# been verified vs. only code-reviewed in this variant.
option(DSD_BUILD_LIQUID_VARIANT "Also build dsd-server-liquid for A/B testing against the hand-rolled demod" ON)

if(DSD_BUILD_LIQUID_VARIANT)
  find_path(LIQUID_INCLUDE_DIR liquid/liquid.h)
  find_library(LIQUID_LIBRARY NAMES liquid)

  if(LIQUID_INCLUDE_DIR AND LIQUID_LIBRARY)
    message(STATUS "liquid-dsp found (${LIQUID_LIBRARY}) - building dsd-server-liquid")

    add_executable(dsd-server-liquid
      src/main.cpp
      src/session.cpp
      src/dsd_process.cpp
      src/fm_demod_liquid.cpp
    )
    target_include_directories(dsd-server-liquid PRIVATE src ${Boost_INCLUDE_DIRS} ${LIQUID_INCLUDE_DIR})
    target_link_libraries(dsd-server-liquid PRIVATE Boost::system Threads::Threads ${LIQUID_LIBRARY} m)
    target_compile_definitions(dsd-server-liquid PRIVATE DSD_USE_LIQUID_DEMOD)
    target_compile_options(dsd-server-liquid PRIVATE -Wall -Wextra)

    add_executable(test_fm_demod_liquid EXCLUDE_FROM_ALL
      src/fm_demod_liquid.cpp
      src/test_fm_demod_liquid.cpp
    )
    target_include_directories(test_fm_demod_liquid PRIVATE src ${LIQUID_INCLUDE_DIR})
    target_link_libraries(test_fm_demod_liquid PRIVATE ${LIQUID_LIBRARY} m)

    # Layer liquid-dsp support onto the always-built demod_benchmark
    # target (defined earlier, alongside test_fm_demod) now that we know
    # liquid was found -- rather than a separate target, so there's one
    # binary that does the full comparison when liquid is available and
    # gracefully falls back to hand-rolled-only otherwise.
    target_sources(demod_benchmark PRIVATE src/fm_demod_liquid.cpp)
    target_include_directories(demod_benchmark PRIVATE ${LIQUID_INCLUDE_DIR})
    target_link_libraries(demod_benchmark PRIVATE ${LIQUID_LIBRARY} m)
    target_compile_definitions(demod_benchmark PRIVATE DSD_HAVE_LIQUID_BENCHMARK)
  else()
    message(STATUS "liquid-dsp not found - skipping dsd-server-liquid. "
                    "Install libliquid-dev (or build from "
                    "https://github.com/jgaeddert/liquid-dsp) to enable it, "
                    "then re-run cmake.")
  endif()
endif()

# --- DSDcc variant, for A/B testing the in-process decoder backend ---
# against the subprocess (dsd-fme/classic DSD) backend. Same
# skip-with-a-message-if-not-found approach as the liquid variant above.
#
# STATUS: verified against DSDcc 1.9.0 built from source with mbelib.
# dsdcc_decoder.cpp's API calls were checked against the real headers
# and dsdccx's own integration loop, and test_dsdcc_decoder (below)
# runs the wrapper on DSDcc's bundled real DMR capture and asserts the
# decoded audio and talkgroup/source metadata match what upstream's
# dsdccx extracts from the same file. The earlier LOW CONFIDENCE
# warning that used to live here (the file was originally written
# without access to DSDcc) no longer applies.
option(DSD_BUILD_DSDCC_VARIANT "Also build dsd-server-dsdcc (in-process DSDcc backend) for testing" ON)

if(DSD_BUILD_DSDCC_VARIANT)
  find_path(DSDCC_INCLUDE_DIR NAMES dsdcc/dsd_decoder.h dsd_decoder.h)
  find_library(DSDCC_LIBRARY NAMES dsdcc)
  find_library(MBE_LIBRARY NAMES mbe) # DSDcc needs mbelib for voice synthesis

  if(DSDCC_INCLUDE_DIR AND DSDCC_LIBRARY)
    message(STATUS "DSDcc found (${DSDCC_LIBRARY}) - building dsd-server-dsdcc")

    add_executable(dsd-server-dsdcc
      src/main.cpp
      src/session.cpp
      src/fm_demod.cpp       # DSDcc variant still uses the original hand-rolled
      src/dsdcc_decoder.cpp  # demod by default -- this isolates the DSD
    )                        # backend as the one variable changed from
                              # baseline. Add -DDSD_USE_LIQUID_DEMOD plus
                              # fm_demod_liquid.cpp instead if you want to
                              # combine both swaps in one binary.
    target_include_directories(dsd-server-dsdcc PRIVATE src ${Boost_INCLUDE_DIRS} ${DSDCC_INCLUDE_DIR})
    target_link_libraries(dsd-server-dsdcc PRIVATE Boost::system Threads::Threads ${DSDCC_LIBRARY})
    if(MBE_LIBRARY)
      target_link_libraries(dsd-server-dsdcc PRIVATE ${MBE_LIBRARY})
    else()
      message(STATUS "mbelib not found - dsd-server-dsdcc will build but "
                      "DSDcc needs it for voice synthesis; install it "
                      "before expecting decoded audio out of this variant.")
    endif()
    target_compile_definitions(dsd-server-dsdcc PRIVATE DSD_USE_DSDCC_BACKEND)
    target_compile_options(dsd-server-dsdcc PRIVATE -Wall -Wextra)

    # Decoder-level test against a real DMR capture -- see
    # test_dsdcc_decoder.cpp for what it asserts and the ground truth.
    # (DSDCC_SAMPLES_DIR is declared near the top of this file, next to
    # DSD_FME_BIN, since the dsd_process test uses the same capture.)

    add_executable(test_dsdcc_decoder EXCLUDE_FROM_ALL
      src/dsdcc_decoder.cpp
      src/test_dsdcc_decoder.cpp
    )
    target_include_directories(test_dsdcc_decoder PRIVATE src ${DSDCC_INCLUDE_DIR})
    target_link_libraries(test_dsdcc_decoder PRIVATE ${DSDCC_LIBRARY})
    if(MBE_LIBRARY)
      target_link_libraries(test_dsdcc_decoder PRIVATE ${MBE_LIBRARY})
    endif()
    target_compile_options(test_dsdcc_decoder PRIVATE -Wall -Wextra)

    if(DSDCC_SAMPLES_DIR AND EXISTS "${DSDCC_SAMPLES_DIR}/dmr_it_8.dis")
      add_test(NAME dsdcc_decoder_test
               COMMAND test_dsdcc_decoder "${DSDCC_SAMPLES_DIR}/dmr_it_8.dis")
      set_tests_properties(dsdcc_decoder_test PROPERTIES TIMEOUT 120)
    endif()

    # Full-stack test of the DSDcc server build: session.cpp compiled
    # with DSD_USE_DSDCC_BACKEND (the dsd-server-dsdcc configuration),
    # driven over a real WebSocket with FM-modulated IQ of the same DMR
    # capture. See test_session_dsdcc.cpp. Needs Boost (like
    # test_session) on top of DSDcc.
    add_executable(test_session_dsdcc EXCLUDE_FROM_ALL
      src/session.cpp
      src/fm_demod.cpp
      src/dsdcc_decoder.cpp
      src/test_session_dsdcc.cpp
    )
    target_include_directories(test_session_dsdcc PRIVATE src ${Boost_INCLUDE_DIRS} ${DSDCC_INCLUDE_DIR})
    target_link_libraries(test_session_dsdcc PRIVATE Boost::system Threads::Threads ${DSDCC_LIBRARY})
    if(MBE_LIBRARY)
      target_link_libraries(test_session_dsdcc PRIVATE ${MBE_LIBRARY})
    endif()
    target_compile_definitions(test_session_dsdcc PRIVATE DSD_USE_DSDCC_BACKEND)
    target_compile_options(test_session_dsdcc PRIVATE -Wall -Wextra)

    if(DSDCC_SAMPLES_DIR AND EXISTS "${DSDCC_SAMPLES_DIR}/dmr_it_8.dis")
      add_test(NAME session_dsdcc_test
               COMMAND test_session_dsdcc "${DSDCC_SAMPLES_DIR}/dmr_it_8.dis")
      # Streaming is deliberately paced (~10 ms per 85 ms block), so the
      # send phase alone takes a few seconds; leave generous headroom.
      set_tests_properties(session_dsdcc_test PROPERTIES TIMEOUT 180)
    endif()

    # Real-signal verification of the liquid demod: modulates the same
    # DMR capture into IQ, demodulates with FmDemodulatorLiquid, and
    # decodes with DsdccDecoder -- needs BOTH DSDcc (this block) and
    # liquid-dsp (whose block above already ran, so its find results are
    # available here). See test_fm_demod_liquid_real.cpp; the round trip
    # is sample-exact against decoding the capture directly.
    if(LIQUID_INCLUDE_DIR AND LIQUID_LIBRARY)
      add_executable(test_fm_demod_liquid_real EXCLUDE_FROM_ALL
        src/fm_demod_liquid.cpp
        src/dsdcc_decoder.cpp
        src/test_fm_demod_liquid_real.cpp
      )
      target_include_directories(test_fm_demod_liquid_real PRIVATE src ${LIQUID_INCLUDE_DIR} ${DSDCC_INCLUDE_DIR})
      target_link_libraries(test_fm_demod_liquid_real PRIVATE ${LIQUID_LIBRARY} ${DSDCC_LIBRARY} m)
      if(MBE_LIBRARY)
        target_link_libraries(test_fm_demod_liquid_real PRIVATE ${MBE_LIBRARY})
      endif()
      target_compile_options(test_fm_demod_liquid_real PRIVATE -Wall -Wextra)

      if(DSDCC_SAMPLES_DIR AND EXISTS "${DSDCC_SAMPLES_DIR}/dmr_it_8.dis")
        add_test(NAME fm_demod_liquid_real_test
                 COMMAND test_fm_demod_liquid_real "${DSDCC_SAMPLES_DIR}/dmr_it_8.dis")
        set_tests_properties(fm_demod_liquid_real_test PROPERTIES TIMEOUT 120)
      endif()
    endif()
  else()
    message(STATUS "DSDcc not found - skipping dsd-server-dsdcc. Build "
                    "from https://github.com/f4exb/dsdcc to enable it, "
                    "then re-run cmake.")
  endif()
endif()

# --- Integration test for session.cpp/main.cpp (the WebSocket server) ---
# Starts a real Server, connects a real WebSocket client, and exercises
# the actual control protocol and audio/event relay end-to-end. Needs
# Boost (same as dsd-server) and a stand-in "dsd-fme" so it doesn't
# require a real DSD binary or a real DMR signal -- see
# test_fake_dsd_fme.cpp and the top-of-file comment in test_session.cpp
# for exactly what this does and doesn't verify.
enable_testing()

add_executable(test-fake-dsd-fme EXCLUDE_FROM_ALL src/test_fake_dsd_fme.cpp)

# test_session.cpp locates its fake DSD backend the same way the real
# Session does: DsdProcessConfig::dsd_fme_path defaults to the literal
# string "dsd-fme", resolved via PATH at fork/exec time. session.cpp
# doesn't currently expose a way to override this per-session, so the
# only lever available to a test is making sure something literally
# named "dsd-fme" is discoverable on PATH -- hence copying the built
# test-fake-dsd-fme binary to a same-directory file named exactly
# "dsd-fme" after each build, rather than just running the binary under
# its more descriptive target name.
add_custom_command(TARGET test-fake-dsd-fme POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy
    $<TARGET_FILE:test-fake-dsd-fme>
    $<TARGET_FILE_DIR:test-fake-dsd-fme>/dsd-fme
  COMMENT "Copying test-fake-dsd-fme to dsd-fme (PATH-based lookup expects this exact name)"
)

add_executable(test_session EXCLUDE_FROM_ALL
  src/session.cpp
  src/dsd_process.cpp
  src/fm_demod.cpp
  src/test_session.cpp
)
target_include_directories(test_session PRIVATE src ${Boost_INCLUDE_DIRS})
target_link_libraries(test_session PRIVATE Boost::system Threads::Threads)
target_compile_options(test_session PRIVATE -Wall -Wextra)
add_dependencies(test_session test-fake-dsd-fme)

add_test(NAME session_integration_test COMMAND test_session)
set_tests_properties(session_integration_test PROPERTIES
  ENVIRONMENT "TEST_FAKE_DSD_FME_DIR=$<TARGET_FILE_DIR:test-fake-dsd-fme>"
  TIMEOUT 60
)

# --- Verification test against a REAL dsd-fme binary ---
# DsdProcess spawning actual dsd-fme (lwvmobile/dsd-fme, built from
# source) and decoding the same real DMR capture the DSDcc tests use --
# this is what verified build_argv's command line and classify_line's
# regexes against reality (see the DSD-FME VERIFICATION NOTES in
# dsd_process.cpp). Needs no Boost. To arm the ctest case, point
# DSD_FME_BIN at a built dsd-fme and DSDCC_SAMPLES_DIR (declared below
# with the DSDcc targets) at a DSDcc source checkout's samples dir;
# without them the binary builds and prints SKIPPED when run bare.
set(DSD_FME_BIN "" CACHE FILEPATH
    "Path to a real dsd-fme binary to enable test_dsd_process's ctest case")
# The real-capture tests (this one and the DSDcc ones below) all use
# DSDcc's bundled DMR capture; it ships in the DSDcc SOURCE tree
# (samples/dmr_it_8.dis), not in its installed artifacts.
set(DSDCC_SAMPLES_DIR "" CACHE PATH
    "Directory containing DSDcc's sample captures (dmr_it_8.dis) to enable the real-capture ctest cases")

add_executable(test_dsd_process EXCLUDE_FROM_ALL
  src/dsd_process.cpp
  src/test_dsd_process.cpp
)
target_include_directories(test_dsd_process PRIVATE src)
target_link_libraries(test_dsd_process PRIVATE Threads::Threads)
target_compile_options(test_dsd_process PRIVATE -Wall -Wextra)

if(DSD_FME_BIN AND EXISTS "${DSD_FME_BIN}" AND DSDCC_SAMPLES_DIR AND EXISTS "${DSDCC_SAMPLES_DIR}/dmr_it_8.dis")
  add_test(NAME dsd_process_test
           COMMAND test_dsd_process "${DSD_FME_BIN}" "${DSDCC_SAMPLES_DIR}/dmr_it_8.dis")
  # Paced streaming (~10 ms per 85 ms block) makes the send phase a few
  # seconds on its own.
  set_tests_properties(dsd_process_test PROPERTIES TIMEOUT 120)
endif()

# --- Full-stack test: stock server + REAL dsd-fme over a WebSocket ---
# test_session's production-configuration twin: same Session/demod/
# DsdProcess build as dsd-server, but the dsd-fme found on PATH is the
# real one and the streamed IQ carries a real DMR signal. See
# test_session_real_fme.cpp. The binary named "dsd-fme" must live in
# the directory containing DSD_FME_BIN (the test passes that directory
# via PATH, exercising the same lookup the server uses in production).
add_executable(test_session_real_fme EXCLUDE_FROM_ALL
  src/session.cpp
  src/dsd_process.cpp
  src/fm_demod.cpp
  src/test_session_real_fme.cpp
)
target_include_directories(test_session_real_fme PRIVATE src ${Boost_INCLUDE_DIRS})
target_link_libraries(test_session_real_fme PRIVATE Boost::system Threads::Threads)
target_compile_options(test_session_real_fme PRIVATE -Wall -Wextra)

if(DSD_FME_BIN AND EXISTS "${DSD_FME_BIN}" AND DSDCC_SAMPLES_DIR AND EXISTS "${DSDCC_SAMPLES_DIR}/dmr_it_8.dis")
  get_filename_component(DSD_FME_DIR "${DSD_FME_BIN}" DIRECTORY)
  add_test(NAME session_real_fme_test
           COMMAND test_session_real_fme "${DSD_FME_DIR}" "${DSDCC_SAMPLES_DIR}/dmr_it_8.dis")
  set_tests_properties(session_real_fme_test PROPERTIES TIMEOUT 180)
endif()

# --- Concurrency stress test for the same server ---
# Separate binary from test_session so the simple end-to-end test stays
# fast and easy to reason about (see the top of test_session.cpp), while
# the threading gets hammered here. Same sources, same fake dsd-fme;
# what differs is the load. Uses its own port, since ctest may run the
# two cases in parallel.
add_executable(test_session_concurrency EXCLUDE_FROM_ALL
  src/session.cpp
  src/dsd_process.cpp
  src/fm_demod.cpp
  src/test_session_concurrency.cpp
)
target_include_directories(test_session_concurrency PRIVATE src ${Boost_INCLUDE_DIRS})
target_link_libraries(test_session_concurrency PRIVATE Boost::system Threads::Threads)
target_compile_options(test_session_concurrency PRIVATE -Wall -Wextra)
add_dependencies(test_session_concurrency test-fake-dsd-fme)

# Longer timeout than the integration test: this one spawns dozens of
# dsd-fme children over its lifetime and deliberately waits out a few
# short read timeouts.
add_test(NAME session_concurrency_test COMMAND test_session_concurrency)
set_tests_properties(session_concurrency_test PROPERTIES
  ENVIRONMENT "TEST_FAKE_DSD_FME_DIR=$<TARGET_FILE_DIR:test-fake-dsd-fme>"
  TIMEOUT 180
)

# --- ThreadSanitizer build of the concurrency test ---
# Off by default: TSan roughly 5-15x's runtime and needs a compiler that
# supports it, so it shouldn't be in anyone's default build. But a
# concurrency test without a race detector mostly proves the server
# doesn't deadlock -- data races are undefined behavior that typically
# does nothing visible on x86, so they pass silently. This target is how
# you actually check for them:
#
#   cmake -DDSD_BUILD_TSAN_TESTS=ON -S . -B build
#   cmake --build build --target test_session_concurrency_tsan
#   TEST_FAKE_DSD_FME_DIR=<build dir> ./build/test_session_concurrency_tsan
#
# -O1 and -g rather than the default Release flags: TSan reports are
# only useful with frame pointers and line numbers, and -O3 inlines the
# interesting frames away.
option(DSD_BUILD_TSAN_TESTS "Also build a ThreadSanitizer version of the concurrency test" OFF)

if(DSD_BUILD_TSAN_TESTS)
  add_executable(test_session_concurrency_tsan EXCLUDE_FROM_ALL
    src/session.cpp
    src/dsd_process.cpp
    src/fm_demod.cpp
    src/test_session_concurrency.cpp
  )
  target_include_directories(test_session_concurrency_tsan PRIVATE src ${Boost_INCLUDE_DIRS})
  target_link_libraries(test_session_concurrency_tsan PRIVATE Boost::system Threads::Threads)
  target_compile_options(test_session_concurrency_tsan PRIVATE
    -Wall -Wextra -fsanitize=thread -fno-omit-frame-pointer -g -O1)
  target_link_options(test_session_concurrency_tsan PRIVATE -fsanitize=thread)
  add_dependencies(test_session_concurrency_tsan test-fake-dsd-fme)
endif()
