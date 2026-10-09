// dsd_backend_selector.hpp
//
// The DSD decoder backend used by session.{hpp,cpp}: the dsd-fme subprocess
// (DsdProcess). This indirection is a thin alias kept so session code reads
// as backend-agnostic; there is a single backend. (A second, in-process
// DSDcc backend existed earlier for A/B comparison and was removed, since it
// could not decode most of what the explorer relies on -- the per-call key
// list, per-network keys, reliable NXDN/P25, EDACS/ProVoice.)

#pragma once

#include "dsd_process.hpp"

namespace dsdsrv {
using ActiveDsdBackend = DsdProcess;
using ActiveDsdBackendConfig = DsdProcessConfig;
}
