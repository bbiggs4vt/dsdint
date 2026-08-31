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
//   "uzsize", "zsize", "frame"           -- speech: sizes + base64(zlib) payload
//   "downlink usage marker"              -- traffic-channel usage marker
//
// Real lines, verbatim from an off-air UK TETRA downlink decoded through our
// demod (MCC/MNC 234/78), abbreviated:
//   {"service":"UPLANE","pdu":"TCH_S","ssi":0,"usage marker":0,
//    "downlink usage marker":62,"encryption mode":0,"uzsize":1380,
//    "zsize":157,"frame":"eJz...=="}                       <- traffic (voice)
//   {"service":"MLE","pdu":"D-NWRK-BROADCAST",...,"cell 0":[{...},...]}  <- broadcast
// (the MLE broadcast nests "cell N" arrays -- the parser skips those cleanly.)
//
// This is the direct analog of tetra_tetmon.hpp for the osmo backend: a pure,
// text-in / DsdEvent-out function, unit-tested (tests/test_tetra_kit_json.cpp)
// without the decoder binary or a live capture. TetraKitProcess spawns the
// decoder and feeds these lines through it.
//
// CONFIDENCE. The keys are confirmed from the decoder source and the object
// is flat (top-level), so the field extraction is exact -- and tolerant: it
// skips nested values (the MLE broadcast's "cell N" arrays, a speech payload)
// rather than choking. The service/pdu -> kind mapping is now **pinned
// against a real off-air capture** (UK TETRA, MCC/MNC 234/78) decoded through
// our own demod: UPLANE/TCH_S = voice, MAC-SYNC = sync, MLE broadcasts =
// unknown. Call-setup (CMCE) mapping stays best-effort until a capture with a
// live call exercises it. The caller-SSI-per-usage-marker association (for
// attributing traffic to a subscriber) is a downstream concern, not this
// parser's.

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
