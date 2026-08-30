// tetra_backend_selector.hpp
//
// Lets the TETRA decode path in session.{hpp,cpp} stay agnostic about which
// TETRA backend is compiled in, exactly as dsd_backend_selector.hpp does for
// the DSD backends. Both TETRA backends are subprocess integrations that
// consume the shared π/4-DQPSK bitstream (src/tetra_demod.*); they differ
// only in how they transport those bits and how they report events:
//
//   * osmo (default): TetraProcess spawns the sq5bpf osmo-tetra `tetra-rx`,
//     feeds bits on stdin, and receives "TETMON" events over UDP.
//   * tetra-kit (future, -DTETRA_USE_TETRAKIT_BACKEND): would spawn
//     tetra-kit's decoder, send bits over UDP:42000, and receive its JSON
//     events over UDP:42100.
//
// Both backends expose the same start/write_bits/stop/running surface and
// emit the shared DsdEvent, so selecting one here needs no session.cpp edit.
// Like the DSD selector, the two backends have different config shapes, so
// session.cpp default-constructs the selected config type (it sets no
// per-field backend options for TETRA) -- so nothing there branches on the
// build either.

#pragma once

#if defined(TETRA_USE_TETRAKIT_BACKEND)
#include "tetra_kit_process.hpp"
namespace dsdsrv {
using ActiveTetraBackend = TetraKitProcess;
using ActiveTetraBackendConfig = TetraKitProcessConfig;
}
#else
#include "tetra_process.hpp"
namespace dsdsrv {
using ActiveTetraBackend = TetraProcess;
using ActiveTetraBackendConfig = TetraProcessConfig;
}
#endif
