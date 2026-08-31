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
// Real lines, verbatim from this fork's tetra-rx decoding an off-air UK
// downlink through our own demod:
//     TETMON_begin FUNC:NETINFO1 CCODE:17 MCC:00ea MNC:004e DLF:0 ULF:0 LA:0 CRYPT:0 RX:1 TETMON_end
//     TETMON_begin FUNC:FREQINFO1 DLF:393087500 LA:6189 RX:1 TETMON_end
//     TETMON_begin FUNC:BURST RX:1 TETMON_end
// and the call-control funcs (from the fork's source; not in that control-
// channel capture): DSETUPDEC/DCONNECTDEC/DRELEASEDEC/DTXGRANTDEC carry
// SSI (+ SSI2 called party), IDX, CID, NID; SDS/DSTATUS carry text. Note
// MCC/MNC are hex (00ea/004e = 234/78). Some datagrams append a binary
// payload after TETMON_end; classify_tetmon_line trims raw to the text.
//
// This is the direct analog of classify_dsd_fme_line(): a pure,
// text-in / DsdEvent-out function, unit-tested (tests/test_tetmon_parse.cpp)
// without the tetra-rx binary. TetraProcess spawns tetra-rx and feeds these
// datagrams through it.
//
// CONFIDENCE. The wrapper grammar and the field extraction are exact. The
// func->kind mapping is **pinned against real captured output** for the funcs
// that appeared (NETINFO1/FREQINFO1/BURST/ENCINFO1 -> sync/unknown) and taken
// from the fork's source for the call-control funcs it didn't
// (DSETUPDEC etc. -> call); a capture with a live call would confirm those.
// parse_tetmon_line() extracts every field generically regardless, so
// tightening the mapping is localized and never affects parsing.

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
