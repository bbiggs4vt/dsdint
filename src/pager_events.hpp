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
//              info_type plus the BIW's own scalar fields. A page whose text
//              looks like ciphertext (looks_encrypted), or a FLEX secure /
//              binary message, also carries payload=encrypted_or_binary.
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

// Heuristic: does this decoded page text look like ciphertext / binary data
// rather than a readable message? multimon-ng decodes every payload as text
// (7-bit alpha, or 4-bit BCD numeric) whether or not it is one, so an
// encrypted page arrives as a normal page whose text is noise. Tuned on real
// encrypted US POCSAG (samples/) and real plaintext pages (0 of 49 flagged):
//   alpha:   >= 8 characters, >= 10% of them control codes other than
//            LF/CR/HT (multimon-ng renders them as "<ETB>", "<SYN>", ...).
//            Uniformly random 7-bit data is ~25% control codes, readable
//            text ~0%. Catches ~90% of random 8-16 char payloads, >94% at 24+,
//            >99% at 40+.
//   numeric: >= 16 characters, >= 12% of them the BCD symbols U [ ] (random
//            nibbles give ~19%; phone numbers / codes ~0%). Weaker -- only 3
//            of 16 symbols are distinctive -- so ~83-89% of random payloads.
// Printable symbols such as | or ~ are NOT counted: pipe-delimited dispatch
// text ("ALERT|FIRE|STN 7") is common and must not be flagged. Conservative
// on purpose: a false alarm would hide a readable page from clients.
bool looks_encrypted(const std::string& text, bool numeric);

// Returns false if the line produces no event (not decoder JSON, a BCH stats
// line, or a system line while forward_system is false).
bool multimon_line_to_event(const std::string& line, DsdEvent& ev, bool forward_system = true);

} // namespace dsdsrv
