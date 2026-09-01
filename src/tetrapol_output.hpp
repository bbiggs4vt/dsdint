// tetrapol_output.hpp
//
// Parser for tetrapol_dump's decoded output -> the shared DsdEvent, so
// session.cpp consumes TETRAPOL like any other backend. It is the TETRAPOL
// analog of tetra_tetmon / classify_dsd_fme_line, with one structural
// difference: tetrapol_dump prints a *multi-line indented tree* per message
// (a `CODOP=0x.. (NAME)` header followed by tab-indented field lines), where
// the other decoders print one line per event. So this parser is STATEFUL --
// it accumulates the field lines under a CODOP header and emits one DsdEvent
// when the next message (or end of stream) arrives.
//
// It also surfaces tetrapol_dump's one-line "unsupported codop 0xNN" reports as
// events: those are PDUs it decoded through FEC/CRC but has no field printer
// for, and they dominate real traffic (e.g. D_GROUP_IDLE 0x58 on a control
// channel), so a codop->name table names and forwards them rather than dropping
// them. Validated on a real off-air capture (sigidwiki), which decoded repeated
// D_GROUP_IDLE messages this way.
//
// Output format pinned to tetrapol-kit (lib/tsdu.c, lib/addr.c) printf strings:
//   CODOP=0x%02x (D_SYSTEM_INFO)      <- message header (message type name)
//   \t\tCOUNTRY_CODE=%d
//   \t\tSYSTEM_ID  \t\t\tNETWORK=%d
//   \t\tLOC_AREA_ID \t\t\tLOC_ID=%d
//   \t\tCELL_ID: BS_ID=%d RWS_ID=%d
//   \t\tCELL_BN=%d
//   \t\tGROUP_ID=%d
//
// Mapping (mirrors the osmo TETMON mapping): D_SYSTEM_INFO and the cell/network
// broadcasts -> kind:"sync" (network identity in `extra`, cell colour has no
// TETRAPOL analog); group-activation / call-setup CODOPs -> kind:"call"
// (GROUP_ID -> talkgroup); everything else -> kind:"unknown" (suppressed by
// default). This is source-pinned; it has NOT been validated against a live
// TETRAPOL signal (no capture in the tree) -- the exact CODOP set and address
// fields are the part to confirm against a real capture / tetrapol_dump.

#pragma once

#include "dsd_backend_types.hpp"

#include <functional>
#include <string>

namespace dsdsrv {

// Emit filter: forward everything with a recognized kind; kind:"unknown"
// (unmapped CODOPs) only when forward_unknown is set. Free function,
// unit-testable, mirroring the other backends' *_forward_event.
bool tetrapol_forward_event(const DsdEvent& ev, bool forward_unknown);

// Stateful accumulator over tetrapol_dump's line output.
class TetrapolParser {
public:
    using EventCallback = std::function<void(const DsdEvent&)>;

    // Feed one line of tetrapol_dump output. A `CODOP=` header finalizes the
    // message in progress (emitting it via `emit`) and starts a new one;
    // indented field lines extend the current message.
    void feed_line(const std::string& line, const EventCallback& emit);

    // Emit any message still being accumulated (call at end of stream).
    void flush(const EventCallback& emit);

    // Discard any in-progress message.
    void reset();

private:
    void finalize(const EventCallback& emit);

    bool have_ = false;      // a message is being accumulated
    DsdEvent cur_;           // the message under construction
    std::string cur_name_;   // CODOP name of the message in progress (for kind)
};

} // namespace dsdsrv
