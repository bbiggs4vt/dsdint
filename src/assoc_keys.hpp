#pragma once
// A per-network encryption keyring the explorer manages and the operator
// hands to their decoder as a dsd-fme key list. Keys are entered per
// (family, network, key id). The server stores the key VALUES but never puts
// them in /net.json or an export -- /net.json advertises only WHICH key ids
// have a key (so the UI can show "key loaded"); the one place a value leaves
// the server is a deliberate download of the key list (/net/keys/list), the
// CSV the operator feeds dsd-fme with -K.
//
// This is for systems you are authorized to monitor. The explorer does not
// decrypt anything itself: the key list is applied by the operator's own
// decoder.
//
// Keyed by the key id as the explorer normalizes it (uppercase hex, no
// leading zeros, at least two digits -- the same form the model's call "kid"
// uses), because a dsd-fme key list is indexed by key id; `alg` is kept for
// display and to size the value. A value is hex (the ciphers here all take a
// hex key; BP/scrambler decimal key lists are out of scope for this list).
#include <cctype>
#include <cstdint>
#include <map>
#include <set>
#include <sstream>
#include <string>

#include "assoc_merge.hpp"   // assocjson::q, mjson::V

namespace dsdsrv {

struct KeyEntry {
    std::string alg;        // the algorithm id it goes with (hex, e.g. "84"); "" if unknown
    std::string value;      // the key, hex (uppercase, spaces stripped)
    std::int64_t added = 0; // when it was set (ms)
};
// family -> network key -> key id -> entry
using KeyRing = std::map<std::string, std::map<std::string, std::map<std::string, KeyEntry>>>;

// Normalize a key id to the explorer's form: uppercase hex, no "0x", no
// leading zeros, at least two digits. "" if it isn't hex.
inline std::string key_norm_id(const std::string& in) {
    std::string s = in;
    if (s.size() > 2 && (s[0] == '0') && (s[1] == 'x' || s[1] == 'X')) s = s.substr(2);
    std::string up;
    for (char c : s) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return std::string();
        up += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    if (up.empty()) return std::string();
    std::size_t nz = up.find_first_not_of('0');
    up = nz == std::string::npos ? std::string() : up.substr(nz);
    while (up.size() < 2) up = "0" + up;
    return up;
}

// Normalize/validate a key value: strip spaces, uppercase. Must be hex and an
// even number of digits, 2..64 (a 256-bit key is 64 hex digits). "" if not.
inline std::string key_norm_value(const std::string& in) {
    std::string up;
    for (char c : in) {
        if (std::isspace(static_cast<unsigned char>(c))) continue;
        if (!std::isxdigit(static_cast<unsigned char>(c))) return std::string();
        up += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    if (up.empty() || up.size() % 2 != 0 || up.size() > 64) return std::string();
    return up;
}

// Set a key (validating id and value). Returns false if either is malformed.
inline bool keyring_set(KeyRing& r, const std::string& fam, const std::string& net,
                        const std::string& kid, const std::string& alg, const std::string& value,
                        std::int64_t now) {
    if (fam.empty() || net.empty()) return false;
    const std::string id = key_norm_id(kid), val = key_norm_value(value);
    if (id.empty() || val.empty()) return false;
    KeyEntry e;
    e.alg = key_norm_id(alg);   // alg is a 1-byte id; same hex normalization
    e.value = val;
    e.added = now;
    r[fam][net][id] = std::move(e);
    return true;
}

inline bool keyring_remove(KeyRing& r, const std::string& fam, const std::string& net,
                           const std::string& kid) {
    const std::string id = key_norm_id(kid);
    auto f = r.find(fam);
    if (f == r.end()) return false;
    auto n = f->second.find(net);
    if (n == f->second.end()) return false;
    if (!n->second.erase(id)) return false;
    if (n->second.empty()) f->second.erase(n);
    if (f->second.empty()) r.erase(f);
    return true;
}

// Is a key set for this id on any of these networks? (The UI passes a
// network and, for a merged group, its parts; a talkgroup/radio its networks.)
inline bool keyring_has(const KeyRing& r, const std::string& fam, const std::string& net,
                        const std::string& kid) {
    const std::string id = key_norm_id(kid);
    auto f = r.find(fam);
    if (f == r.end()) return false;
    auto n = f->second.find(net);
    if (n == f->second.end()) return false;
    return n->second.count(id) != 0;
}

// The whole ring with values, for the keys file. Never sent to a browser.
inline std::string keyring_json(const KeyRing& r) {
    using assocjson::q;
    std::ostringstream o;
    o << "{";
    bool ff = true;
    for (const auto& fk : r) {
        o << (ff ? "" : ",") << q(fk.first) << ":{"; ff = false;
        bool fn = true;
        for (const auto& nk : fk.second) {
            o << (fn ? "" : ",") << q(nk.first) << ":{"; fn = false;
            bool fi = true;
            for (const auto& ik : nk.second) {
                const KeyEntry& e = ik.second;
                o << (fi ? "" : ",") << q(ik.first) << ":{\"alg\":" << q(e.alg)
                  << ",\"key\":" << q(e.value) << ",\"added\":" << e.added << "}";
                fi = false;
            }
            o << "}";
        }
        o << "}";
    }
    return o.str() + "}";
}

// Which key ids are set per (family, network), with the algorithm -- NO values.
// For /net.json, so the explorer can show "key loaded" and name the algorithm
// (even for a key id not yet heard in a call). {"p25":{"<net>":{"309":"84","666A":"84"}}}
inline std::string keyring_loaded_json(const KeyRing& r) {
    using assocjson::q;
    std::ostringstream o;
    o << "{";
    bool ff = true;
    for (const auto& fk : r) {
        o << (ff ? "" : ",") << q(fk.first) << ":{"; ff = false;
        bool fn = true;
        for (const auto& nk : fk.second) {
            o << (fn ? "" : ",") << q(nk.first) << ":{"; fn = false;
            bool fi = true;
            for (const auto& ik : nk.second) { o << (fi ? "" : ",") << q(ik.first) << ":" << q(ik.second.alg); fi = false; }
            o << "}";
        }
        o << "}";
    }
    return o.str() + "}";
}

// Load a ring from a keys file written by keyring_json.
inline KeyRing keyring_parse(const mjson::V& root) {
    KeyRing r;
    const mjson::V* keys = root.get("keys");
    if (!keys || keys->t != mjson::V::Obj) return r;
    for (const auto& fk : keys->o) {
        if (fk.second.t != mjson::V::Obj) continue;
        for (const auto& nk : fk.second.o) {
            if (nk.second.t != mjson::V::Obj) continue;
            for (const auto& ik : nk.second.o) {
                if (ik.second.t != mjson::V::Obj) continue;
                const std::string id = key_norm_id(ik.first);
                const std::string val = key_norm_value(ik.second.str("key"));
                if (id.empty() || val.empty()) continue;
                KeyEntry e;
                e.alg = key_norm_id(ik.second.str("alg"));
                e.value = val;
                const mjson::V* a = ik.second.get("added");
                e.added = a && a->t == mjson::V::Num ? static_cast<std::int64_t>(a->n) : 0;
                r[fk.first][nk.first][id] = std::move(e);
            }
        }
    }
    return r;
}

// The dsd-fme hex key list (-K) for one network: a header row (dsd-fme skips
// it) then "keyid,key" per key. A key longer than 64 bits (AES) is split into
// 64-bit (16 hex) columns, which is how dsd-fme's -K importer reads the extra
// fields. Decimal (-k) BP/scrambler lists are not produced here.
inline std::string keyring_csv(const KeyRing& r, const std::string& fam, const std::string& net) {
    std::ostringstream o;
    o << "KEY ID,KEY\n";
    auto f = r.find(fam);
    if (f == r.end()) return o.str();
    auto n = f->second.find(net);
    if (n == f->second.end()) return o.str();
    for (const auto& ik : n->second) {
        o << ik.first;
        const std::string& v = ik.second.value;
        for (std::size_t i = 0; i < v.size(); i += 16) o << "," << v.substr(i, 16);
        o << "\n";
    }
    return o.str();
}

// A dsd-fme hex key list (-K) covering every network of `fam` -- or, when
// `fam` is empty, every family -- so the server can hand its own decoder the
// keyring. One "keyid,key" row per distinct key id (same 64-bit-column split
// as keyring_csv). A dsd-fme -K list is indexed by key id alone, so where two
// networks use the same key id with different keys only the first is emitted
// (a collision the UI can warn about); within one network, key ids are unique.
inline std::string keyring_csv_family(const KeyRing& r, const std::string& fam) {
    std::ostringstream o;
    o << "KEY ID,KEY\n";
    std::set<std::string> seen;
    for (const auto& fk : r) {
        if (!fam.empty() && fk.first != fam) continue;
        for (const auto& nk : fk.second) {
            for (const auto& ik : nk.second) {
                if (!seen.insert(ik.first).second) continue;  // key id already emitted
                o << ik.first;
                const std::string& v = ik.second.value;
                for (std::size_t i = 0; i < v.size(); i += 16) o << "," << v.substr(i, 16);
                o << "\n";
            }
        }
    }
    return o.str();
}

} // namespace dsdsrv
