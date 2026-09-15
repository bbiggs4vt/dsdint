// protocol_capabilities.hpp
//
// Builds the {"type":"capabilities", ...} frame the server sends once,
// immediately after the WebSocket opens (before any "start"). It lets a
// client discover PROGRAMMATICALLY what this particular server build can
// put on the wire -- the event kinds, and the `extra` token keys it may
// emit -- instead of hard-coding them from PROTOCOL.md.
//
// The set of `extra` keys depends on both the compiled DSD backend
// (dsd-fme subprocess vs in-process DSDcc, chosen at build time via
// dsd_backend_selector.hpp) and the protocol. At connect time the client
// hasn't chosen a protocol yet, so the keys are grouped by protocol
// family (extra_keys_dmr, extra_keys_p25, ...) and pre-filtered to the
// keys THIS backend can actually produce; the client reads the field for
// whichever protocol it's about to request. Families this build can never
// emit a key for are omitted entirely.
//
// This mirrors PROTOCOL.md's "extra token vocabulary" table; keep the two
// in sync when tokens are added or removed.

#pragma once

#include "json_util.hpp"

#include <string>
#include <vector>

namespace dsdsrv {

// Which backend a given `extra` key comes from. `Both` = either DSD
// backend; `Tetra` = the TETRA chain (a separate subprocess path present
// in both builds), so it's always available regardless of the DSD choice.
enum class KeyBackend { Fme, Dsdcc, Both, Tetra };

#if defined(DSD_USE_DSDCC_BACKEND)
inline constexpr KeyBackend kActiveDsdBackend = KeyBackend::Dsdcc;
inline constexpr const char* kDsdBackendName = "dsdcc";
#else
inline constexpr KeyBackend kActiveDsdBackend = KeyBackend::Fme;
inline constexpr const char* kDsdBackendName = "dsd-fme";
#endif

inline bool cap_key_active(KeyBackend b) {
    // Both/Tetra are always live; a backend-specific key only when this is
    // the compiled DSD backend.
    return b == KeyBackend::Both || b == KeyBackend::Tetra || b == kActiveDsdBackend;
}

struct CapKey { const char* key; KeyBackend backend; };
struct CapFamily { const char* field; std::vector<CapKey> keys; };

// The `protocol` hint values this build actually decodes. Hints that a
// backend only accepts by falling back to auto (provoice / edacs* /
// x2tdma on DSDcc) or decodes without metadata (p25 on DSDcc) are tagged
// to the backend that truly handles them, so each build advertises only
// what it can really do. The TETRA hints ride the separate TETRA chain,
// present in both builds. Mirrors ProtocolHint / parse_protocol_hint in
// session.cpp and the protocol list in PROTOCOL.md.
inline const std::vector<CapKey>& cap_protocols() {
    static const std::vector<CapKey> protos = {
        {"dmr",           KeyBackend::Both},
        {"nxdn48",        KeyBackend::Both},
        {"nxdn96",        KeyBackend::Both},
        {"dpmr",          KeyBackend::Both},
        {"dstar",         KeyBackend::Both},
        {"ysf",           KeyBackend::Both},
        {"p25",           KeyBackend::Fme},
        {"p25p2",         KeyBackend::Fme},
        {"provoice",      KeyBackend::Fme},
        {"edacs",         KeyBackend::Fme},
        {"edacs_esk",     KeyBackend::Fme},
        {"edacs_ea",      KeyBackend::Fme},
        {"edacs_ea_esk",  KeyBackend::Fme},
        {"x2tdma",        KeyBackend::Fme},
        {"tetra",         KeyBackend::Tetra},
        {"tetrakit",      KeyBackend::Tetra},
        {"auto",          KeyBackend::Both},
    };
    return protos;
}

// The `extra` token vocabulary, grouped by protocol family (mirrors the
// table in PROTOCOL.md). Order within a family follows the doc table.
inline const std::vector<CapFamily>& cap_families() {
    static const std::vector<CapFamily> fams = {
        {"extra_keys_dmr", {
            {"unit_target",  KeyBackend::Dsdcc},
            {"burst",        KeyBackend::Dsdcc},
            {"sync_type",    KeyBackend::Dsdcc},
            {"network_type", KeyBackend::Fme},
            {"network_id",   KeyBackend::Fme},
            {"site_id",      KeyBackend::Fme},
            {"rest_channel", KeyBackend::Fme},
            {"lcn",          KeyBackend::Fme},
        }},
        {"extra_keys_p25", {
            {"rfss",      KeyBackend::Fme},
            {"site_id",   KeyBackend::Fme},
            {"system_id", KeyBackend::Fme},
            {"wacn",      KeyBackend::Fme},
            {"alg_id",    KeyBackend::Fme},
            {"key_id",    KeyBackend::Fme},
        }},
        {"extra_keys_nxdn", {
            {"site_code",   KeyBackend::Both},
            {"system_code", KeyBackend::Both},
            {"location_id", KeyBackend::Both},
            {"category",    KeyBackend::Fme},
        }},
        {"extra_keys_dstar", {
            {"rpt1",       KeyBackend::Both},
            {"rpt2",       KeyBackend::Both},
            {"radio_text", KeyBackend::Both},
            {"gps",        KeyBackend::Dsdcc},
        }},
        {"extra_keys_ysf", {
            {"uplink",    KeyBackend::Both},
            {"downlink",  KeyBackend::Both},
            {"call_mode", KeyBackend::Both},
            {"data_type", KeyBackend::Both},
            {"src_rid",   KeyBackend::Both},
            {"dst_rid",   KeyBackend::Both},
        }},
        {"extra_keys_edacs", {
            {"lcn",       KeyBackend::Fme},
            {"afs",       KeyBackend::Fme},
            {"lid",       KeyBackend::Fme},
            {"system_id", KeyBackend::Fme},
        }},
        {"extra_keys_tetra", {
            {"mcc",              KeyBackend::Tetra},
            {"mnc",              KeyBackend::Tetra},
            {"la",               KeyBackend::Tetra},
            {"dlf",              KeyBackend::Tetra},
            {"ulf",              KeyBackend::Tetra},
            {"crypt",            KeyBackend::Tetra},
            {"cid",              KeyBackend::Tetra},
            {"nid",              KeyBackend::Tetra},
            {"idx",              KeyBackend::Tetra},
            {"status",           KeyBackend::Tetra},
            {"afc",              KeyBackend::Tetra},
            {"func",             KeyBackend::Tetra},
            {"service",          KeyBackend::Tetra},
            {"pdu",              KeyBackend::Tetra},
            {"usage_marker",     KeyBackend::Tetra},
            {"dl_usage_marker",  KeyBackend::Tetra},
            {"encr",             KeyBackend::Tetra},
        }},
    };
    return fams;
}

// "; "-join the keys in a family that this backend can emit ("" if none).
inline std::string cap_join_family(const CapFamily& fam) {
    std::string out;
    for (const auto& k : fam.keys) {
        if (!cap_key_active(k.backend)) continue;
        if (!out.empty()) out += "; ";
        out += k.key;
    }
    return out;
}

// The full capabilities frame as a flat JSON string. Families with no
// key this build can emit are omitted.
inline std::string build_capabilities_json() {
    json::Writer w;
    w.field("type", std::string("capabilities"));
    w.field("backend", std::string(kDsdBackendName));
    // Protocol hints this build actually decodes ("; "-joined).
    std::string protos;
    for (const auto& p : cap_protocols()) {
        if (!cap_key_active(p.backend)) continue;
        if (!protos.empty()) protos += "; ";
        protos += p.key;
    }
    w.field("protocols", protos);
    // Audio shape is now uniform across backends (see PROTOCOL.md).
    w.field("audio", std::string("pcm_s16le_8000_mono"));
    w.field("event_kinds", std::string("voice; sync; call; message; burst; unknown"));
    for (const auto& fam : cap_families()) {
        std::string keys = cap_join_family(fam);
        if (!keys.empty()) w.field(fam.field, keys);
    }
    return w.str();
}

} // namespace dsdsrv
