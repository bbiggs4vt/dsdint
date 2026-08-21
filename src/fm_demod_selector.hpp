// fm_demod_selector.hpp
//
// Lets session.{hpp,cpp} stay backend-agnostic: build with
// -DDSD_USE_LIQUID_DEMOD to get the liquid-dsp-based demodulator,
// otherwise you get the hand-rolled one from fm_demod.cpp. Both
// implementations expose the same interface (see fm_demod.hpp's
// FmDemodulator class), so this is a pure compile-time swap — nothing
// else in the server changes between the two builds, which is what
// makes an apples-to-apples A/B comparison on target hardware possible:
// same networking code, same dsd-fme plumbing, only the DSP core differs.
//
// See CMakeLists.txt for the two targets this produces:
//   dsd-server          (original hand-rolled demod, fm_demod.cpp)
//   dsd-server-liquid    (liquid-dsp demod, fm_demod_liquid.cpp; only
//                         built if liquid-dsp is found on your system)

#pragma once

#if defined(DSD_USE_LIQUID_DEMOD)
#include "fm_demod_liquid.hpp"
namespace dsdsrv { using ActiveFmDemodulator = FmDemodulatorLiquid; }
#else
#include "fm_demod.hpp"
namespace dsdsrv { using ActiveFmDemodulator = FmDemodulator; }
#endif
