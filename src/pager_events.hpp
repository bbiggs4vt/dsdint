// pager_events.hpp
//
// Maps one line of multimon-ng `--json` output (POCSAG / FLEX_NEXT) onto the
// server's shared DsdEvent, so paging rides the exact same flat `event` frame
// as every voice protocol. A pure text-in / DsdEvent-out function, unit-tested
// in tests/test_pager_events.cpp against real decoder output.
//
// Mapping (see PROTOCOL.md "Paging"):
//   kind       "page" for a pager message; "sync" for FLEX control-channel
//              system info (BIW date/time/system id -- network broadcasts,
//              like TETRA's NETINFO); FLEX BCH statistics are dropped.
//   talkgroup  the pager address: POCSAG RIC / FLEX capcode. A page's
//              addressee is the closest analog of a talkgroup.
//   message    the page text ("" for tone-only pages).
//   emergency  "1" for a FLEX priority message.
//   crc_error  "1" when FLEX reports a failed message checksum (k_ok=false).
//   extra      "; "-joined key=value tokens: protocol, baud, message_type,
//              function (POCSAG), and for FLEX flex_type, levels, phase,
//              cycle, frame, addr_type, group, fragment; system events carry
//              info_type plus the BIW's own scalar fields.
//   raw        the original multimon-ng JSON line.
//
// multimon-ng's FLEX objects nest a few values (group_capcodes, flextime);
// the tolerant flat parser shared with the tetra-kit backend skips those, and
// they remain available verbatim in `raw`.

#pragma once

#include "dsd_backend_types.hpp"

#include <string>

namespace dsdsrv {

// Strips POCSAG fill padding rendered by multimon-ng ("<NUL>", "<EOT>",
// "<ETX>", "<ETB>") and trailing whitespace from the end of a message.
std::string trim_pager_padding(std::string s);

// Returns false if the line produces no event (not decoder JSON, a BCH stats
// line, or a system line while forward_system is false).
bool multimon_line_to_event(const std::string& line, DsdEvent& ev, bool forward_system = true);

} // namespace dsdsrv
