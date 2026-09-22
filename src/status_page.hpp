// status_page.hpp
//
// Renders the server status page from a ServerStats::Snapshot: an HTML view
// for a browser (GET / or /status) and a JSON view for tooling
// (GET /status.json). Pure formatting -- no locks, no I/O; the caller takes
// the snapshot and hands it here.
//
// The HTML page auto-refreshes with a <meta> refresh so an operator can
// leave it open; the JSON endpoint is what a dashboard or health check
// should poll.

#pragma once

#include "server_stats.hpp"

#include <ctime>
#include <sstream>
#include <string>

namespace dsdsrv {

// How often the HTML page tells the browser to reload itself.
inline constexpr int kStatusRefreshSeconds = 5;

namespace status_detail {

inline std::string html_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += c;
        }
    }
    return out;
}

inline std::string json_escape(const std::string& s) {
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

// "1h 23m 45s" / "23m 45s" / "45s"
inline std::string human_duration(double seconds) {
    long s = static_cast<long>(seconds);
    if (s < 0) s = 0;
    long h = s / 3600; s %= 3600;
    long m = s / 60;   s %= 60;
    std::ostringstream o;
    if (h) o << h << "h ";
    if (h || m) o << m << "m ";
    o << s << "s";
    return o.str();
}

// UTC ISO-8601-ish "2026-09-22 14:03:21Z" for a wall-clock time point.
inline std::string format_utc(std::chrono::system_clock::time_point tp) {
    std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm_utc{};
#if defined(_WIN32)
    gmtime_s(&tm_utc, &t);
#else
    gmtime_r(&t, &tm_utc);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%SZ", &tm_utc);
    return buf;
}

} // namespace status_detail

inline std::string render_status_json(const ServerStats::Snapshot& s) {
    using namespace status_detail;
    std::ostringstream o;
    o << "{";
    o << "\"total_sessions\":" << s.total_sessions;
    o << ",\"current_sessions\":" << s.current_sessions;
    o << ",\"active_pipelines\":" << s.active_pipelines;
    o << ",\"uptime_seconds\":" << static_cast<long>(s.uptime_s);
    o << ",\"started\":\"" << json_escape(format_utc(s.started)) << "\"";

    o << ",\"by_protocol\":{";
    bool first = true;
    for (const auto& kv : s.by_protocol) {
        if (!first) o << ",";
        first = false;
        o << "\"" << json_escape(kv.first) << "\":" << kv.second;
    }
    o << "}";

    o << ",\"sessions\":[";
    for (std::size_t i = 0; i < s.rows.size(); ++i) {
        const auto& r = s.rows[i];
        if (i) o << ",";
        double dur = std::chrono::duration<double>(
                         std::chrono::steady_clock::now() - r.connected_mono).count();
        o << "{";
        o << "\"id\":" << r.id;
        o << ",\"remote\":\"" << json_escape(r.remote) << "\"";
        o << ",\"protocol\":\"" << json_escape(r.protocol) << "\"";
        o << ",\"chain\":\"" << json_escape(r.chain) << "\"";
        o << ",\"active\":" << (r.active ? "true" : "false");
        o << ",\"connected\":\"" << json_escape(format_utc(r.connected)) << "\"";
        o << ",\"duration_seconds\":" << static_cast<long>(dur);
        o << "}";
    }
    o << "]";
    o << "}";
    return o.str();
}

inline std::string render_status_html(const ServerStats::Snapshot& s) {
    using namespace status_detail;
    std::ostringstream o;
    o << "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n"
      << "<meta charset=\"utf-8\">\n"
      << "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
      << "<meta http-equiv=\"refresh\" content=\"" << kStatusRefreshSeconds << "\">\n"
      << "<title>dsd-server status</title>\n"
      << "<style>\n"
      << "  :root { color-scheme: light dark; }\n"
      << "  body { font-family: -apple-system, Segoe UI, Roboto, Helvetica, Arial, sans-serif;\n"
      << "         margin: 0; padding: 1.5rem; line-height: 1.4; }\n"
      << "  h1 { font-size: 1.25rem; margin: 0 0 0.25rem; }\n"
      << "  .sub { color: #888; font-size: 0.85rem; margin-bottom: 1.25rem; }\n"
      << "  .cards { display: flex; flex-wrap: wrap; gap: 0.75rem; margin-bottom: 1.5rem; }\n"
      << "  .card { border: 1px solid #8883; border-radius: 8px; padding: 0.75rem 1rem;\n"
      << "          min-width: 8rem; }\n"
      << "  .card .n { font-size: 1.6rem; font-weight: 600; }\n"
      << "  .card .l { color: #888; font-size: 0.8rem; text-transform: uppercase;\n"
      << "             letter-spacing: 0.03em; }\n"
      << "  table { border-collapse: collapse; width: 100%; font-size: 0.9rem; }\n"
      << "  th, td { text-align: left; padding: 0.4rem 0.6rem; border-bottom: 1px solid #8882; }\n"
      << "  th { color: #888; font-weight: 600; font-size: 0.78rem; text-transform: uppercase;\n"
      << "       letter-spacing: 0.03em; }\n"
      << "  td.num { text-align: right; font-variant-numeric: tabular-nums; }\n"
      << "  .dot { display: inline-block; width: 0.55rem; height: 0.55rem; border-radius: 50%;\n"
      << "         margin-right: 0.35rem; vertical-align: baseline; }\n"
      << "  .on { background: #2ecc71; } .off { background: #bbb; }\n"
      << "  .empty { color: #888; padding: 0.75rem 0; }\n"
      << "  .tag { font-variant-numeric: tabular-nums; }\n"
      << "</style>\n</head>\n<body>\n";

    o << "<h1>dsd-server status</h1>\n";
    o << "<div class=\"sub\">up " << html_escape(human_duration(s.uptime_s))
      << " &middot; since " << html_escape(format_utc(s.started))
      << " &middot; refreshes every " << kStatusRefreshSeconds << "s</div>\n";

    // Summary cards.
    o << "<div class=\"cards\">\n";
    auto card = [&](const std::string& label, const std::string& value) {
        o << "  <div class=\"card\"><div class=\"n\">" << html_escape(value)
          << "</div><div class=\"l\">" << html_escape(label) << "</div></div>\n";
    };
    card("Current sessions", std::to_string(s.current_sessions));
    card("Total sessions", std::to_string(s.total_sessions));
    card("Active decodes", std::to_string(s.active_pipelines));
    o << "</div>\n";

    // Active-pipeline breakdown by protocol.
    if (!s.by_protocol.empty()) {
        o << "<div class=\"sub\">active by protocol: ";
        bool first = true;
        for (const auto& kv : s.by_protocol) {
            if (!first) o << " &middot; ";
            first = false;
            o << "<span class=\"tag\">" << html_escape(kv.first) << " "
              << kv.second << "</span>";
        }
        o << "</div>\n";
    }

    // Session table.
    o << "<table>\n<thead><tr>"
      << "<th>#</th><th>Client</th><th>Protocol</th><th>Chain</th>"
      << "<th>State</th><th>Connected (UTC)</th><th class=\"num\">Duration</th>"
      << "</tr></thead>\n<tbody>\n";
    if (s.rows.empty()) {
        o << "<tr><td colspan=\"7\" class=\"empty\">no clients connected</td></tr>\n";
    } else {
        for (const auto& r : s.rows) {
            double dur = std::chrono::duration<double>(
                             std::chrono::steady_clock::now() - r.connected_mono).count();
            o << "<tr>"
              << "<td class=\"num\">" << r.id << "</td>"
              << "<td>" << html_escape(r.remote.empty() ? "-" : r.remote) << "</td>"
              << "<td>" << html_escape(r.protocol) << "</td>"
              << "<td>" << html_escape(r.chain.empty() ? "-" : r.chain) << "</td>"
              << "<td><span class=\"dot " << (r.active ? "on" : "off") << "\"></span>"
              << (r.active ? "decoding" : "idle") << "</td>"
              << "<td>" << html_escape(format_utc(r.connected)) << "</td>"
              << "<td class=\"num\">" << html_escape(human_duration(dur)) << "</td>"
              << "</tr>\n";
        }
    }
    o << "</tbody>\n</table>\n</body>\n</html>\n";
    return o.str();
}

} // namespace dsdsrv
