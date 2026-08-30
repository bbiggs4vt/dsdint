// tetra_kit_json.hpp
//
// Parser for the JSON reports emitted by tetra-kit's decoder
// (gitlab.com/larryth/tetra-kit), the second TETRA backend. tetra-kit's
// decoder reads the demodulated bitstream over UDP (its -r port; unpacked,
// one bit per byte -- exactly what src/tetra_demod.* produces) and sends one
// JSON object per line over UDP (its -t port) describing each decoded PDU.
//
// The report is a FLAT JSON object (built with RapidJSON in the decoder's
// common/report.cc). The key names below are taken verbatim from that file:
//
//   "time", "service", "pdu",            -- what was decoded
//   "tn", "fn", "mn",                    -- TETRA timeslot / frame / multiframe
//   "ssi", "address_type",              -- subscriber identity + its form
//   "actual ssi", "ussi", "smi",         -- address variants (per address_type)
//   "usage marker", "actual usage marker",
//   "encryption mode",
//   "uzsize", "zsize"                    -- speech: uncompressed/zlib sizes
//
// e.g. (illustrative, real shape):
//   {"service":"CMCE","pdu":"D-SETUP","ssi":1234567,"usage marker":3,"tn":1,"fn":3}
//
// This is the direct analog of tetra_tetmon.hpp for the osmo backend: a pure,
// text-in / DsdEvent-out function, unit-tested (tests/test_tetra_kit_json.cpp)
// without the decoder binary or a live capture. TetraKitProcess spawns the
// decoder and feeds these lines through it.
//
// CONFIDENCE. The keys above are confirmed from the decoder source and the
// object is flat, so the field extraction is exact (and tolerant: it skips
// any nested/array value, e.g. a speech data array, rather than choking).
// The pdu->kind classification and which identity maps to source vs talkgroup
// are the best current reading and are expected to be pinned against real
// tetra-kit output -- the same posture taken for the osmo TETMON funcs.

#pragma once

#include "dsd_backend_types.hpp"

#include <string>
#include <map>

namespace dsdsrv {

// Flat top-level scalar fields of one tetra-kit JSON report. Values are kept
// as strings (numbers stringified). Nested objects/arrays are skipped. valid
// is false if the line is not a JSON object.
struct TetraKitReport {
    bool valid = false;
    std::map<std::string, std::string> fields;
};

// Parse one JSON line/datagram from tetra-kit's decoder. Best-effort and
// tolerant of interior whitespace and of nested/array values (which are
// skipped, not stored). Returns {valid=false} if there is no JSON object.
TetraKitReport parse_tetrakit_json(const std::string& line);

// Map a parsed report to the shared DsdEvent. PROVISIONAL semantics (see
// CONFIDENCE): field extraction is exact, but the pdu->kind table and id
// assignment are the best current reading, to be pinned against real output.
// `raw` always carries the original JSON line.
DsdEvent tetrakit_to_event(const TetraKitReport& rep, const std::string& raw_line);

// parse + map in one call; kind "unknown", raw=line for a non-JSON line.
DsdEvent classify_tetrakit_json(const std::string& line);

} // namespace dsdsrv
