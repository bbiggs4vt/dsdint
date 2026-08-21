// dsd_backend_selector.hpp
//
// Lets session.{hpp,cpp} stay backend-agnostic between the subprocess
// model (DsdProcess, driving dsd-fme or classic DSD) and the in-process
// DSDcc model (DsdccDecoder). Build with -DDSD_USE_DSDCC_BACKEND to get
// the latter. Mirrors fm_demod_selector.hpp's approach for the demod
// backend -- same reasoning applies: this is what keeps the A/B
// comparison apples-to-apples, since everything except this one type
// alias stays identical between builds.
//
// NOTE: unlike the demod backends (which share the exact same
// FmDemodConfig type), the two DSD backends have meaningfully different
// config shapes (DsdProcessConfig has dsd_fme_path/mode_flag/
// extra_args/udp_audio_port; DsdccConfig has mode/input_sample_rate_hz),
// because they're genuinely different integration models, not just
// different implementations of the same interface. session.cpp has to
// branch with #ifdef DSD_USE_DSDCC_BACKEND when *populating* the config
// struct, even though it doesn't need to branch when *using* the
// resulting object (start/write_audio/stop/running are named the same
// on both).

#pragma once

#if defined(DSD_USE_DSDCC_BACKEND)
#include "dsdcc_decoder.hpp"
namespace dsdsrv {
using ActiveDsdBackend = DsdccDecoder;
using ActiveDsdBackendConfig = DsdccConfig;
}
#else
#include "dsd_process.hpp"
namespace dsdsrv {
using ActiveDsdBackend = DsdProcess;
using ActiveDsdBackendConfig = DsdProcessConfig;
}
#endif
