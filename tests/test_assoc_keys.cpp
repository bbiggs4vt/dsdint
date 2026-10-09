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

    if (g_failures == 0) { std::printf("\nALL KEYRING TESTS PASSED\n"); return 0; }
    std::printf("\n%d KEYRING TEST(S) FAILED\n", g_failures);
    return 1;
}
