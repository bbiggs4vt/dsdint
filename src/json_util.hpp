// json_util.hpp
//
// Deliberately minimal JSON writer + parser. This project's messages are
// small, flat, known-shape objects (control commands in, event/telemetry
// records out), so a hand-rolled implementation avoids pulling in a full
// JSON library dependency. If your event schema grows (nested objects,
// arrays of records, etc.) swap this for nlohmann::json or similar rather
// than extending this by hand.

#pragma once

#include <cstdio>
#include <string>
#include <sstream>
#include <unordered_map>
#include <variant>
#include <cctype>
#include <stdexcept>

namespace dsdsrv::json {

// ---- Writing -----------------------------------------------------------

class Writer {
public:
    Writer() { buf_ += '{'; }
    // Explicitly defaulted: buf_ is a plain std::string, so Writer is
    // naturally copyable/movable as long as nothing here reintroduces a
    // non-copyable member (this used to hold a std::ostringstream
    // directly, which made Writer non-copyable and broke any call site
    // that tried `auto w = Writer().field(...)...;` -- field() returns
    // Writer&, and that auto-copy-initialization needed a copy
    // constructor that didn't exist. Number formatting below uses a
    // local ostringstream per call instead of a stored one, so that
    // problem can't come back.
    Writer(const Writer&) = default;
    Writer(Writer&&) = default;
    Writer& operator=(const Writer&) = default;
    Writer& operator=(Writer&&) = default;

    Writer& field(const std::string& key, const std::string& value) {
        sep();
        buf_ += '"'; buf_ += escape(key); buf_ += "\":\"";
        buf_ += escape(value); buf_ += '"';
        return *this;
    }
    Writer& field(const std::string& key, double value) {
        sep();
        buf_ += '"'; buf_ += escape(key); buf_ += "\":";
        buf_ += format_number(value);
        return *this;
    }
    Writer& field(const std::string& key, long long value) {
        sep();
        buf_ += '"'; buf_ += escape(key); buf_ += "\":";
        buf_ += std::to_string(value);
        return *this;
    }
    Writer& field(const std::string& key, bool value) {
        sep();
        buf_ += '"'; buf_ += escape(key); buf_ += "\":";
        buf_ += (value ? "true" : "false");
        return *this;
    }
    // Pass through an already-JSON-encoded value verbatim (e.g. nested
    // object built with another Writer).
    Writer& raw_field(const std::string& key, const std::string& raw_json) {
        sep();
        buf_ += '"'; buf_ += escape(key); buf_ += "\":";
        buf_ += raw_json;
        return *this;
    }

    std::string str() const { return buf_ + "}"; }

private:
    void sep() {
        if (!first_) buf_ += ',';
        first_ = false;
    }
    // Local stream per call, not a stored member -- keeps the same
    // stream-default number formatting as before (e.g. "2402" rather
    // than std::to_string's "2402.000000") without making Writer itself
    // hold a non-copyable ostringstream.
    static std::string format_number(double value) {
        std::ostringstream tmp;
        tmp << value;
        return tmp.str();
    }
    static std::string escape(const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (char c : s) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    // RFC 8259: control characters below 0x20 MUST be
                    // escaped. This actually happens in practice -- real
                    // dsd-fme colorizes its log with ANSI sequences, so
                    // raw event lines contain ESC (0x1b); unescaped, that
                    // produced invalid JSON the browser side rejects.
                    if (static_cast<unsigned char>(c) < 0x20) {
                        char u[8];
                        std::snprintf(u, sizeof(u), "\\u%04x", c);
                        out += u;
                    } else {
                        out += c;
                    }
            }
        }
        return out;
    }
    std::string buf_;
    bool first_ = true;
};

// ---- Parsing -------------------------------------------------------------
// Flat-object parser only: {"key": "string"|number|true|false, ...}.
// Throws std::runtime_error on malformed input. Good enough for this
// project's control messages; not a general-purpose JSON parser.

using Value = std::variant<std::string, double, bool>;

inline std::unordered_map<std::string, Value> parse_flat_object(const std::string& s) {
    std::unordered_map<std::string, Value> out;
    std::size_t i = 0;
    auto skip_ws = [&]() { while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i; };

    skip_ws();
    if (i >= s.size() || s[i] != '{') throw std::runtime_error("json: expected '{'");
    ++i;
    skip_ws();
    if (i < s.size() && s[i] == '}') return out; // empty object

    while (true) {
        skip_ws();
        if (i >= s.size() || s[i] != '"') throw std::runtime_error("json: expected key string");
        ++i;
        std::string key;
        while (i < s.size() && s[i] != '"') {
            if (s[i] == '\\' && i + 1 < s.size()) { key += s[i + 1]; i += 2; }
            else { key += s[i]; ++i; }
        }
        ++i; // closing quote
        skip_ws();
        if (i >= s.size() || s[i] != ':') throw std::runtime_error("json: expected ':'");
        ++i;
        skip_ws();

        if (i < s.size() && s[i] == '"') {
            ++i;
            std::string val;
            while (i < s.size() && s[i] != '"') {
                if (s[i] == '\\' && i + 1 < s.size()) { val += s[i + 1]; i += 2; }
                else { val += s[i]; ++i; }
            }
            ++i;
            out[key] = val;
        } else if (s.compare(i, 4, "true") == 0) {
            out[key] = true; i += 4;
        } else if (s.compare(i, 5, "false") == 0) {
            out[key] = false; i += 5;
        } else {
            std::size_t start = i;
            while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) ||
                                     s[i] == '-' || s[i] == '+' || s[i] == '.' ||
                                     s[i] == 'e' || s[i] == 'E')) ++i;
            if (i == start) throw std::runtime_error("json: expected value");
            out[key] = std::stod(s.substr(start, i - start));
        }

        skip_ws();
        if (i < s.size() && s[i] == ',') { ++i; continue; }
        if (i < s.size() && s[i] == '}') { ++i; break; }
        throw std::runtime_error("json: expected ',' or '}'");
    }
    return out;
}

inline std::string get_string(const std::unordered_map<std::string, Value>& obj,
                               const std::string& key, const std::string& def = "") {
    auto it = obj.find(key);
    if (it == obj.end()) return def;
    if (auto p = std::get_if<std::string>(&it->second)) return *p;
    return def;
}

inline double get_number(const std::unordered_map<std::string, Value>& obj,
                          const std::string& key, double def = 0.0) {
    auto it = obj.find(key);
    if (it == obj.end()) return def;
    if (auto p = std::get_if<double>(&it->second)) return *p;
    return def;
}

} // namespace dsdsrv::json
