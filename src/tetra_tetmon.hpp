// tetra_tetmon.hpp
//
// Parser for the "TETMON" UDP text protocol emitted by the sq5bpf fork of
// osmo-tetra (github.com/sq5bpf/osmo-tetra-sq5bpf), the osmo lineage we
// target for TETRA. That fork's `tetra-rx` reads the demodulated bitstream
// (one unpacked bit per byte -- exactly what src/tetra_demod.* +
// tools/tetra_bit_source produce) on stdin and reports decoder activity by
// sending short UDP text datagrams to TETRA_HACK_IP:TETRA_HACK_PORT. Each
// datagram is one message wrapped as:
//
//     TETMON_begin FUNC:<name> <KEY:VALUE> <KEY:VALUE> ... TETMON_end
//
// e.g. the AFC status line, taken verbatim from the fork's tetra-rx.c:
//
//     TETMON_begin FUNC:AFCVAL AFC:-3 RX:1 TETMON_end
//
// and a call-control example (field set per telive's documentation):
//
//     TETMON_begin FUNC:SETUPDEC IDX:12 SSI:1234567 CID:5 NID:9 RX:1 TETMON_end
//
// This is the direct analog of classify_dsd_fme_line(): a pure,
// text-in / DsdEvent-out function so it is unit-testable on its own
// (tests/test_tetmon_parse.cpp) without the tetra-rx binary or a live
// capture. The TetraProcess backend that spawns tetra-rx and receives these
// datagrams over UDP is a later increment; it will lean on this parser.
//
// CONFIDENCE. The wrapper grammar (TETMON_begin ... TETMON_end, space-
// separated KEY:VALUE tokens) and the AFCVAL field set are confirmed against
// the fork's source. The broader FUNC vocabulary and the exact id-field
// semantics (which SSI is a group GSSI vs an individual ISSI, etc.) are
// derived from telive's documentation, not yet pinned against captured
// output -- the same posture we took for dsd-fme's un-captured line formats.
// parse_tetmon_line() extracts every field generically regardless, so
// tightening the func->kind / id mapping later is a small, localized change
// that does not affect parsing.

#pragma once

#include "dsd_backend_types.hpp"

#include <string>
#include <map>

namespace dsdsrv {

// The generic result of parsing one TETMON datagram: the FUNC name plus
// every KEY:VALUE field (FUNC included in `fields` too). `valid` is false if
// the line is not a well-formed TETMON_begin...TETMON_end message. This
// layer is format-exact and fully tested; it commits to no TETRA semantics.
struct TetmonMessage {
    bool valid = false;
    std::string func;                            // value of FUNC: (may be "")
    std::map<std::string, std::string> fields;   // all KEY:VALUE, incl. FUNC
};

// Parse one line/datagram of TETMON text. Tolerant of surrounding and
// interior whitespace; ignores anything before TETMON_begin or after
// TETMON_end. Returns {valid=false} if the wrapper markers are absent.
TetmonMessage parse_tetmon_line(const std::string& line);

// Map a parsed TETMON message to the shared DsdEvent shape session.cpp
// consumes. PROVISIONAL semantics (see CONFIDENCE above): the field
// extraction is exact, but the func->kind table and id assignment are the
// best current reading and are expected to be pinned against real tetra-rx
// output. `raw` always carries the original line so nothing is lost.
DsdEvent tetmon_to_event(const TetmonMessage& msg, const std::string& raw_line);

// Convenience: parse + map in one call. Returns an event with kind
// "unknown" and raw=line for anything that isn't a valid TETMON message, so
// callers can forward-or-filter exactly as the dsd-fme path does.
DsdEvent classify_tetmon_line(const std::string& line);

} // namespace dsdsrv
