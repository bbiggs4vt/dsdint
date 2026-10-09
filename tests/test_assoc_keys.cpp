// Keyring (assoc_keys.hpp): normalization, set/remove, the loaded-ids JSON
// (no values), the whole-ring JSON (with values), round-trip, and the dsd-fme
// -K key list. Also that AssocModel never puts a key value in /net.json.
#include <cstdio>
#include <filesystem>
#include <string>

#include <unistd.h>

#include "assoc_keys.hpp"
#include "assoc_model.hpp"

using namespace dsdsrv;

static int g_failures = 0;
static void check(bool c, const std::string& what) {
    std::printf("  %s: %s\n", c ? "OK" : "FAIL", what.c_str());
    if (!c) ++g_failures;
}

int main() {
    std::printf("test_assoc_keys\n");

    // ---- normalization ----
    check(key_norm_id("0x0309") == "309" && key_norm_id("309") == "309" && key_norm_id("1") == "01",
          "key id: 0x / leading zeros stripped, min two digits");
    check(key_norm_id("") == "" && key_norm_id("xyz") == "", "key id: non-hex rejected");
    check(key_norm_value("01 23 45 67") == "01234567" && key_norm_value("abCD") == "ABCD",
          "key value: spaces stripped, uppercased");
    check(key_norm_value("ABC") == "" && key_norm_value("zz") == "" &&
              key_norm_value(std::string(66, 'A')) == "",
          "key value: odd length / non-hex / over 64 digits rejected");

    // ---- set / remove ----
    {
        KeyRing r;
        check(keyring_set(r, "p25", "nac:201", "0x666A", "84",
                          "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF", 1000),
              "set: a valid 256-bit key");
        check(!keyring_set(r, "p25", "nac:201", "1", "84", "ABC", 1000), "set: a bad value is rejected");
        check(!keyring_set(r, "p25", "", "1", "84", "AABB", 1000), "set: an empty network is rejected");
        check(keyring_has(r, "p25", "nac:201", "666a"), "has: found (id normalized case-insensitively)");
        check(!keyring_has(r, "p25", "nac:201", "0001") && !keyring_has(r, "dmr", "nac:201", "666A"),
              "has: a different id / family is not found");
        check(keyring_remove(r, "p25", "nac:201", "0x666A") && !keyring_has(r, "p25", "nac:201", "666A"),
              "remove: gone, and the empty family/network pruned");
        check(!keyring_remove(r, "p25", "nac:201", "666A"), "remove: a missing key returns false");
    }

    // ---- loaded-ids JSON carries NO values; ring JSON does ----
    {
        KeyRing r;
        keyring_set(r, "p25", "nac:201", "666A", "84", "0011223344556677", 1000);
        keyring_set(r, "p25", "nac:201", "0309", "84", "8899AABBCCDDEEFF", 1000);
        const std::string loaded = keyring_loaded_json(r);
        check(loaded.find("\"666A\"") != std::string::npos && loaded.find("\"309\"") != std::string::npos,
              "loaded json: lists the key ids");
        check(loaded.find("0011223344556677") == std::string::npos,
              "loaded json: never contains a key value");
        check(keyring_json(r).find("0011223344556677") != std::string::npos,
              "ring json: does contain the value (the keys file)");

        // round-trip through the file JSON
        mjson::V root;
        check(mjson::parse("{\"keys\":" + keyring_json(r) + "}", root), "ring json: parses");
        KeyRing r2 = keyring_parse(root);
        check(keyring_has(r2, "p25", "nac:201", "666A") && keyring_has(r2, "p25", "nac:201", "309"),
              "round-trip: both keys come back");
    }

    // ---- dsd-fme -K key list (hex, 64-bit columns) ----
    {
        KeyRing r;
        keyring_set(r, "p25", "nac:201", "666A", "84",
                    "A1B2C3D4E5F60708A1B2C3D4E5F60708A1B2C3D4E5F60708A1B2C3D4E5F60708", 1000);
        keyring_set(r, "p25", "nac:201", "0C", "AA", "1122334455", 1000);   // 40-bit RC4-ish
        const std::string csv = keyring_csv(r, "p25", "nac:201");
        check(csv.rfind("KEY ID,KEY\n", 0) == 0, "csv: header row (dsd-fme skips it)");
        check(csv.find("666A,A1B2C3D4E5F60708,A1B2C3D4E5F60708,A1B2C3D4E5F60708,A1B2C3D4E5F60708") != std::string::npos,
              "csv: a 256-bit key is split into four 64-bit columns");
        check(csv.find("0C,1122334455\n") != std::string::npos, "csv: a short key is one column");
        check(keyring_csv(r, "p25", "nope").find("KEY ID,KEY") == 0 && keyring_csv(r, "p25", "nope").size() < 14,
              "csv: an unknown network is just the header");
    }

    // ---- family-wide -K key list (what the server hands its own decoder) ----
    {
        KeyRing r;
        keyring_set(r, "p25", "nac:201", "666A", "84", "A1B2C3D4E5F60708", 1000);
        keyring_set(r, "p25", "nac:777", "0C", "AA", "1122334455", 1000);     // another p25 network
        keyring_set(r, "p25", "nac:777", "666A", "84", "DEADBEEFDEADBEEF", 1000); // same id as nac:201
        keyring_set(r, "dmr", "cc:1", "05", "25", "0011223344556677", 1000);  // a different family
        const std::string p = keyring_csv_family(r, "p25");
        check(p.rfind("KEY ID,KEY\n", 0) == 0, "family csv: header row");
        check(p.find("666A,A1B2C3D4E5F60708") != std::string::npos && p.find("0C,1122334455") != std::string::npos,
              "family csv: every network's key ids are included");
        check(p.find("DEADBEEFDEADBEEF") == std::string::npos,
              "family csv: a key id shared across networks is emitted once (first wins)");
        check(p.find("0011223344556677") == std::string::npos, "family csv: another family's keys are excluded");
        const std::string all = keyring_csv_family(r, "");
        check(all.find("0011223344556677") != std::string::npos && all.find("A1B2C3D4E5F60708") != std::string::npos,
              "family csv: an empty family covers every family");
        check(keyring_csv_family(r, "nxdn") == "KEY ID,KEY\n", "family csv: a family with no keys is just the header");
    }

    // ---- the model stores keys and never leaks a value into /net.json ----
    {
        AssocModel m;
        check(m.set_key("p25", "nac:201@408200000", "666A", "84",
                        "DEADBEEFCAFEBABEDEADBEEFCAFEBABEDEADBEEFCAFEBABEDEADBEEFCAFEBABE"),
              "model: set_key stores a valid key");
        check(!m.set_key("p25", "nac:201@408200000", "1", "84", "nothex"), "model: a bad key is refused");
        const std::string j = m.to_json(2000);
        check(j.find("\"keyed\":") != std::string::npos && j.find("666A") != std::string::npos,
              "model: /net.json advertises the key id under \"keyed\"");
        check(j.find("DEADBEEFCAFEBABE") == std::string::npos,
              "model: /net.json never contains the key value");
        check(m.keys_csv("p25", "nac:201@408200000").find("DEADBEEFCAFEBABE") != std::string::npos,
              "model: the key list download does carry the value");
        check(m.remove_key("p25", "nac:201@408200000", "666A") &&
                  m.to_json(2100).find("666A") == std::string::npos,
              "model: remove_key clears it from \"keyed\"");
    }

    // ---- DMR Basic Privacy (key number per network, applied by frequency) ----
    {
        AssocModel m;
        check(m.set_bp("dmr", "cc:1@440425000", 7), "bp: a valid key number (1-255) is set");
        check(!m.set_bp("dmr", "cc:1@440425000", 0) && !m.set_bp("dmr", "cc:1@440425000", 256) &&
                  !m.set_bp("dmr", "", 7),
              "bp: out-of-range numbers and an empty network are rejected");
        check(m.to_json(2000).find("\"bp\":{\"dmr\":{\"cc:1@440425000\":7}}") != std::string::npos,
              "bp: /net.json advertises the number (it is not secret)");
        check(m.bp_for_freq("dmr", 440425000) == 7, "bp: matched by the network's frequency");
        check(m.bp_for_freq("dmr", 460175000) == 0 && m.bp_for_freq("p25", 440425000) == 0 &&
                  m.bp_for_freq("dmr", 0) == 0,
              "bp: no match for another frequency / family / no frequency");

        check(m.remove_bp("dmr", "cc:1@440425000") && m.bp_for_freq("dmr", 440425000) == 0 &&
                  !m.remove_bp("dmr", "cc:1@440425000"),
              "bp: remove clears it, and removing again is false");

        // Round-trip through the keys file: the number persists across a load.
        namespace fs = std::filesystem;
        const std::string kf = (fs::temp_directory_path() / ("dsd_bp_keys_" + std::to_string(::getpid()) + ".json")).string();
        fs::remove(kf);
        AssocModel w;
        w.use_keys_file(kf);                       // sets the path (file absent yet); set_bp then writes it
        w.set_bp("dmr", "cc:5@451237500", 42);
        AssocModel r;
        check(r.use_keys_file(kf) && r.bp_for_freq("dmr", 451237500) == 42,
              "bp: it is kept in the keys file and comes back on load");
        fs::remove(kf);
    }

    // ---- DMR Enhanced Privacy (a secret hex key per network, any key id) ----
    {
        AssocModel m;
        check(m.set_net_key("ep", "dmr", "cc:1@440425000", "1a2b3c4d5e"), "ep: a 10-hex-digit key is set");
        check(m.set_net_key("ep", "dmr", "cc:2@451000000", "abc") && m.net_key_for_freq("ep", "dmr", 451000000) == "0000000ABC",
              "ep: a shorter key is zero-padded to 40 bits");
        check(!m.set_net_key("ep", "dmr", "cc:1@440425000", "") && !m.set_net_key("ep", "dmr", "cc:1@440425000", "0000") &&
                  !m.set_net_key("ep", "dmr", "cc:1@440425000", "123456789AB") && !m.set_net_key("ep", "dmr", "cc:1@440425000", "xyz") &&
                  !m.set_net_key("ep", "dmr", "", "01"),
              "ep: empty / zero / over 40-bit / non-hex keys and an empty network are rejected");
        const std::string js = m.to_json(2000);
        check(js.find("\"ep\":{\"dmr\":[\"cc:1@440425000\",\"cc:2@451000000\"]}") != std::string::npos &&
                  js.find("1A2B3C4D5E") == std::string::npos,
              "ep: /net.json lists the networks but never the key value");
        check(m.net_key_for_freq("ep", "dmr", 440425000) == "1A2B3C4D5E", "ep: matched by the network's frequency");
        check(m.net_key_for_freq("ep", "dmr", 460175000).empty() && m.net_key_for_freq("ep", "p25", 440425000).empty() &&
                  m.net_key_for_freq("ep", "dmr", 0).empty(),
              "ep: no match for another frequency / family / no frequency");

        // The stream's -K list: the EP key for every 8-bit key id, then the
        // keyring (so a key-id key overrides EP for its own id).
        m.set_key("dmr", "cc:1@440425000", "05", "24", "00112233445566778899AABBCCDDEEFF");
        const std::string csv = m.keys_csv_stream("dmr", "1A2B3C4D5E");
        check(csv.rfind("KEY ID,KEY\n00,1A2B3C4D5E\n", 0) == 0 && csv.find("\nFF,1A2B3C4D5E\n") != std::string::npos &&
                  csv.find("KEY ID") == csv.rfind("KEY ID"),
              "ep: the stream key list has the EP key for ids 00-FF and one header");
        check(csv.find("\nFF,1A2B3C4D5E\n05,0011223344556677,8899AABBCCDDEEFF\n") != std::string::npos,
              "ep: keyring keys follow the EP rows (later rows win in dsd-fme)");
        check(m.keys_csv_stream("dmr", "") == m.keys_csv_family("dmr"), "ep: no EP key -> the plain family list");

        check(m.remove_net_key("ep", "dmr", "cc:1@440425000") && m.net_key_for_freq("ep", "dmr", 440425000).empty() &&
                  !m.remove_net_key("ep", "dmr", "cc:1@440425000"),
              "ep: remove clears it, and removing again is false");

        namespace fs = std::filesystem;
        const std::string kf = (fs::temp_directory_path() / ("dsd_ep_keys_" + std::to_string(::getpid()) + ".json")).string();
        fs::remove(kf);
        AssocModel w;
        w.use_keys_file(kf);
        w.set_net_key("ep", "dmr", "cc:5@451237500", "0102030405");
        w.set_bp("dmr", "cc:5@451237500", 9);
        AssocModel r;
        check(r.use_keys_file(kf) && r.net_key_for_freq("ep", "dmr", 451237500) == "0102030405" &&
                  r.bp_for_freq("dmr", 451237500) == 9,
              "ep: it is kept in the keys file (beside BP) and comes back on load");
        fs::remove(kf);
    }

    // ---- TYT-style Enhanced Privacy (AES-128, 32 hex digits, per network) ----
    {
        AssocModel m;
        check(m.set_net_key("tytep", "dmr", "cc:1@440425000", "00000000000000000000000000012345") &&
                  m.net_key_for_freq("tytep", "dmr", 440425000) == "00000000000000000000000000012345",
              "tytep: a 32-hex-digit key is set and matched by frequency");
        check(m.set_net_key("tytep", "dmr", "cc:2@451000000", "12345") &&
                  m.net_key_for_freq("tytep", "dmr", 451000000) == "00000000000000000000000000012345",
              "tytep: a shorter key is zero-padded to 128 bits");
        check(m.set_net_key("tytep", "dmr", "cc:3@452000000", "736B9A9C5645288B 243AD5CB8701EF8A") &&
                  m.net_key_for_freq("tytep", "dmr", 452000000) == "736B9A9C5645288B243AD5CB8701EF8A",
              "tytep: dsd-fme's space-separated form is accepted");
        check(!m.set_net_key("tytep", "dmr", "cc:1@440425000", std::string(33, '1')) &&
                  !m.set_net_key("tytep", "dmr", "cc:1@440425000", "0") && !m.set_net_key("tytep", "dmr", "cc:1@440425000", "g1") &&
                  !m.set_net_key("tytep", "dmr", "", "1"),
              "tytep: over 128-bit / zero / non-hex keys and an empty network are rejected");
        check(m.net_key_for_freq("ep", "dmr", 440425000).empty() && m.net_key_for_freq("tytep", "dmr", 460175000).empty() &&
                  m.net_key_for_freq("tytep", "p25", 440425000).empty(),
              "tytep: kept apart from Motorola EP; no match for another frequency / family");
        const std::string js = m.to_json(2000);
        check(js.find("\"tytep\":{\"dmr\":[\"cc:1@440425000\",\"cc:2@451000000\",\"cc:3@452000000\"]}") != std::string::npos &&
                  js.find("12345") == std::string::npos && js.find("736B9A9C") == std::string::npos,
              "tytep: /net.json lists the networks but never the key value");
        check(m.remove_net_key("tytep", "dmr", "cc:1@440425000") && m.net_key_for_freq("tytep", "dmr", 440425000).empty() &&
                  !m.remove_net_key("tytep", "dmr", "cc:1@440425000"),
              "tytep: remove clears it, and removing again is false");

        namespace fs = std::filesystem;
        const std::string kf = (fs::temp_directory_path() / ("dsd_tytep_keys_" + std::to_string(::getpid()) + ".json")).string();
        fs::remove(kf);
        AssocModel w;
        w.use_keys_file(kf);
        w.set_net_key("tytep", "dmr", "cc:5@451237500", "00000000000000000000000000012345");
        w.set_net_key("ep", "dmr", "cc:5@451237500", "0102030405");
        AssocModel r;
        check(r.use_keys_file(kf) && r.net_key_for_freq("tytep", "dmr", 451237500) == "00000000000000000000000000012345" &&
                  r.net_key_for_freq("ep", "dmr", 451237500) == "0102030405",
              "tytep: it is kept in the keys file (beside EP) and comes back on load");
        fs::remove(kf);
    }

    // ---- Anytone BP and TYT / Baofeng / Retevis AP (forced per-network keys) ----
    {
        AssocModel m;
        check(m.set_net_key("anybp", "dmr", "cc:1@440425000", "1a2") &&
                  m.net_key_for_freq("anybp", "dmr", 440425000) == "01A2",
              "anybp: a 16-bit key is set and zero-padded to 4 digits");
        check(!m.set_net_key("anybp", "dmr", "cc:1@440425000", "12345") && !m.set_net_key("anybp", "dmr", "cc:1@440425000", "0"),
              "anybp: over 16 bits / zero are rejected");
        const std::string k128 = "736B9A9C5645288B243AD5CB8701EF8A";
        const std::string k256 = "1122334455667788" "99AABBCCDDEEFF11" "1122334455667788" "99AABBCCDDEEFF11";
        for (const char* kind : {"tytap", "bfap", "rtap"}) {
            const std::string K = kind;
            AssocModel a;
            check(a.set_net_key(K, "dmr", "cc:2@451000000", "736B9A9C5645288B 243AD5CB8701EF8A") &&
                      a.net_key_for_freq(K, "dmr", 451000000) == k128,
                  K + ": a 128-bit key (dsd-fme's spaced form) is set");
            check(a.set_net_key(K, "dmr", "cc:2@451000000", k256) && a.net_key_for_freq(K, "dmr", 451000000) == k256,
                  K + ": a 256-bit key (64 digits) is set");
            check(a.set_net_key(K, "dmr", "cc:2@451000000", "ABC") &&
                      a.net_key_for_freq(K, "dmr", 451000000) == std::string(29, '0') + "ABC",
                  K + ": a short key is zero-padded to 128 bits");
            check(!a.set_net_key(K, "dmr", "cc:2@451000000", std::string(40, '1')) &&
                      !a.set_net_key(K, "dmr", "cc:2@451000000", std::string(65, '1')),
                  K + ": 33-63 or over 64 digits are rejected (128 or 256 bits only)");
        }
        check(!m.set_net_key("nope", "dmr", "cc:1@440425000", "12") && !AssocModel::net_key_kind("bp"),
              "kinds: an unknown kind is rejected (BP has its own store)");

        // One forced kind per network: setting another replaces it; Motorola
        // EP (not forced) is unaffected.
        check(m.set_net_key("ep", "dmr", "cc:1@440425000", "0102030405") &&
                  m.set_net_key("tytap", "dmr", "cc:1@440425000", k128) &&
                  m.net_key_for_freq("anybp", "dmr", 440425000).empty() &&
                  m.net_key_for_freq("ep", "dmr", 440425000) == "0102030405",
              "forced: setting TYT AP clears the network's Anytone BP, keeps its Motorola EP");
        const auto fk = m.forced_net_key_for_freq("dmr", 440425000);
        check(fk.first == "tytap" && fk.second == k128 && m.forced_net_key_for_freq("dmr", 460175000).first.empty(),
              "forced: the stream's forced key is found by frequency");
        const std::string js = m.to_json(2000);
        check(js.find("\"tytap\":{\"dmr\":[\"cc:1@440425000\"]}") != std::string::npos &&
                  js.find("\"anybp\":{}") != std::string::npos && js.find("736B9A9C") == std::string::npos,
              "forced: /net.json lists networks per kind, never values");

        namespace fs = std::filesystem;
        const std::string kf = (fs::temp_directory_path() / ("dsd_fk_keys_" + std::to_string(::getpid()) + ".json")).string();
        fs::remove(kf);
        AssocModel w;
        w.use_keys_file(kf);
        w.set_net_key("bfap", "dmr", "cc:5@451237500", k256);
        w.set_net_key("anybp", "dmr", "cc:6@452000000", "BEEF");
        AssocModel r;
        check(r.use_keys_file(kf) && r.net_key_for_freq("bfap", "dmr", 451237500) == k256 &&
                  r.net_key_for_freq("anybp", "dmr", 452000000) == "BEEF",
              "forced: kept in the keys file and back on load");
        fs::remove(kf);
    }

    if (g_failures == 0) { std::printf("\nALL KEYRING TESTS PASSED\n"); return 0; }
    std::printf("\n%d KEYRING TEST(S) FAILED\n", g_failures);
    return 1;
}
