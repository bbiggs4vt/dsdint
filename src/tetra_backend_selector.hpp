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
// Only osmo exists today, so this header currently resolves to it
// unconditionally; the #ifdef seam is here so adding the tetra-kit backend
// is a localized change (a new TetraKitProcess with the same
// start/write_bits/stop/running surface, selected here) rather than a
// session.cpp edit. Like the DSD selector, the two backends may have
// different config shapes, so session.cpp branches on the build when
// *populating* the config but not when *using* the object.

#pragma once

#if defined(TETRA_USE_TETRAKIT_BACKEND)
// #include "tetra_kit_process.hpp"   // not yet implemented
// namespace dsdsrv {
// using ActiveTetraBackend = TetraKitProcess;
// using ActiveTetraBackendConfig = TetraKitProcessConfig;
// }
#error "TETRA_USE_TETRAKIT_BACKEND is not implemented yet; build the osmo backend (default)."
#else
#include "tetra_process.hpp"
namespace dsdsrv {
using ActiveTetraBackend = TetraProcess;
using ActiveTetraBackendConfig = TetraProcessConfig;
}
#endif
