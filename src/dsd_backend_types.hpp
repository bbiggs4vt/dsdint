// dsd_backend_types.hpp
//
// Types shared by every DSD backend (subprocess-based DsdProcess and the
// in-process DsdccDecoder), so session.cpp can consume events the same
// way regardless of which backend produced them.

#pragma once

#include <string>

namespace dsdsrv {

struct DsdEvent {
    std::string raw_line;      // subprocess backends: original stdout line. dsdcc: a synthesized description.
    std::string kind;          // best-effort classification, e.g. "voice", "sync", "call", "unknown"
    std::string talkgroup;     // parsed TG/dst id if present
    std::string source_id;     // parsed source/radio id if present
    std::string slot;          // TDMA slot (0/1) if present
    std::string color_code;    // DMR color code if present, decimal without leading zeros
    std::string extra;         // any other parsed detail, free-form
};

} // namespace dsdsrv
