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
    std::string ran;           // NXDN Radio Access Number if present, decimal without leading zeros
    std::string nac;           // P25 Network Access Code if present, uppercase hex without "0x"
    std::string emergency;     // "1" when the line marks the call an emergency, else ""
    std::string alias;         // DMR talker alias text if present, else "" (free text)
    std::string crc_error;     // "1" when the decoder marked this line/burst as failing CRC/FEC; "" internally when the line carried no CRC/FEC marker (reported as "0" on the wire -- see crc_error_wire)
    std::string message;       // DMR short-data / SMS text (free text) if present, else ""
    std::string extra;         // any other parsed detail, free-form ("; "-joined key=value tokens)
};

// Wire form of the crc_error flag. Internally the field is "" when a line
// carried no CRC/FEC marker, which is ambiguous next to the "1" failure flag;
// on the wire we report a definite 0/1 instead of a blank so clients always
// see a boolean. Note "0" means "not flagged as an error", not "verified to
// have passed a CRC" -- many lines simply don't carry CRC status.
inline std::string crc_error_wire(const std::string& v) {
    return v == "1" ? std::string("1") : std::string("0");
}

} // namespace dsdsrv
