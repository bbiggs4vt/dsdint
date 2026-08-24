# Cross-compile for aarch64 and run tests under QEMU user-mode emulation.
#
#   sudo apt install g++-12-aarch64-linux-gnu qemu-user
#   cmake -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-linux-gnu.toolchain.cmake \
#         -S . -B build-arm64 [the usual DSDCC/DSD_FME/SAMPLES options]
#   cmake --build build-arm64 --target <tests>
#
# CROSSCOMPILING_EMULATOR makes ctest wrap every test in qemu-aarch64
# automatically. Adjust CMAKE_FIND_ROOT_PATH for wherever your arm64
# builds of mbelib/DSDcc are installed.
#
# What emulation does and does not tell you: it validates CORRECTNESS on
# aarch64 (a full run of this suite passes, with the real-capture decode
# tests sample-exact against native x86 -- 151680 / 314880, the same
# counts), but NOT performance: QEMU does not model the microarchitecture,
# so NEON-vs-scalar ratios and any benchmark numbers under emulation are
# meaningless. Run demod_benchmark on real ARM hardware for those.
#
# Two practicalities for emulated runs:
#  - Without binfmt_misc, a QEMU-emulated server cannot exec the arm64
#    dsd-fme child directly. Bridge it with a host shell script named
#    "dsd-fme" that execs `qemu-aarch64 -L /usr/aarch64-linux-gnu
#    <arm64 dsd-fme> "$@"`, and point TEST_FAKE_DSD_FME_DIR / the test
#    path arguments at the directory holding that script.
#  - QEMU runs this pipeline ~40x slower than native; set
#    DSD_TEST_PACE_MS=100 for the paced full-stack tests so their
#    senders don't outrun the emulated demod (see those tests).

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc-12)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++-12)
set(CMAKE_FIND_ROOT_PATH /usr/aarch64-linux-gnu /opt/arm64-prefix /usr/lib/aarch64-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_CROSSCOMPILING_EMULATOR "qemu-aarch64;-L;/usr/aarch64-linux-gnu")
