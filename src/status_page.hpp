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
      << "  :root {\n"
      << "    --bg: #0d1117; --panel: #161b22; --panel2: #12161c; --border: #263040;\n"
      << "    --text: #e6edf3; --muted: #8b949e; --accent: #3fb950; --accent2: #58a6ff;\n"
      << "    color-scheme: dark;\n"
      << "  }\n"
      << "  * { box-sizing: border-box; }\n"
      << "  html, body { background: var(--bg); }\n"
      << "  body { color: var(--text); margin: 0; padding: 0 0 2rem;\n"
      << "         font-family: -apple-system, Segoe UI, Roboto, Helvetica, Arial, sans-serif;\n"
      << "         line-height: 1.4; }\n"
      << "  .wrap { max-width: 1100px; margin: 0 auto; padding: 0 1.25rem; }\n"
      << "  header { background: linear-gradient(180deg, #1b2230, #12161c);\n"
      << "           border-bottom: 2px solid var(--accent); margin-bottom: 1.5rem; }\n"
      << "  header .wrap { padding-top: 1rem; padding-bottom: 1rem; }\n"
      << "  h1 { font-size: 1.15rem; margin: 0; letter-spacing: 0.02em;\n"
      << "       text-transform: uppercase; font-weight: 700; }\n"
      << "  h1 .accent { color: var(--accent); }\n"
      << "  .sub { color: var(--muted); font-size: 0.82rem; margin-top: 0.3rem; }\n"
      << "  .cards { display: flex; flex-wrap: wrap; gap: 0.9rem; margin-bottom: 1.5rem; }\n"
      << "  .card { background: var(--panel); border: 1px solid var(--border);\n"
      << "          border-radius: 10px; padding: 0.9rem 1.15rem; min-width: 9rem; }\n"
      << "  .card .n { font-size: 1.9rem; font-weight: 700; color: var(--accent2);\n"
      << "             font-variant-numeric: tabular-nums; }\n"
      << "  .card .l { color: var(--muted); font-size: 0.72rem; text-transform: uppercase;\n"
      << "             letter-spacing: 0.06em; margin-top: 0.1rem; }\n"
      << "  .panel { background: var(--panel); border: 1px solid var(--border);\n"
      << "           border-radius: 10px; overflow: hidden; }\n"
      << "  table { border-collapse: collapse; width: 100%; font-size: 0.88rem; }\n"
      << "  th, td { text-align: left; padding: 0.55rem 0.85rem; }\n"
      << "  thead th { background: var(--panel2); color: var(--muted); font-weight: 600;\n"
      << "             font-size: 0.7rem; text-transform: uppercase; letter-spacing: 0.06em;\n"
      << "             border-bottom: 1px solid var(--border); }\n"
      << "  tbody tr { border-top: 1px solid #1c232d; }\n"
      << "  tbody tr:nth-child(even) { background: #0f141a; }\n"
      << "  tbody tr:hover { background: #1a2230; }\n"
      << "  td.num { text-align: right; font-variant-numeric: tabular-nums; }\n"
      << "  td.mono { font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;\n"
      << "            color: #c9d1d9; }\n"
      << "  .dot { display: inline-block; width: 0.55rem; height: 0.55rem; border-radius: 50%;\n"
      << "         margin-right: 0.4rem; vertical-align: baseline; }\n"
      << "  .on { background: var(--accent); box-shadow: 0 0 6px #3fb95088; }\n"
      << "  .off { background: #59636e; }\n"
      << "  .state-on { color: var(--accent); }\n"
      << "  .state-off { color: var(--muted); }\n"
      << "  .empty { color: var(--muted); padding: 1rem 0.85rem; }\n"
      << "  .tag { display: inline-block; background: #1b2634; border: 1px solid var(--border);\n"
      << "         border-radius: 999px; padding: 0.05rem 0.55rem; margin-right: 0.35rem;\n"
      << "         font-size: 0.8rem; font-variant-numeric: tabular-nums; }\n"
      << "  .tag b { color: var(--accent2); font-weight: 700; }\n"
      << "  .section-label { color: var(--muted); font-size: 0.72rem; text-transform: uppercase;\n"
      << "                   letter-spacing: 0.06em; margin: 0 0 0.6rem; }\n"
      << "</style>\n</head>\n<body>\n";

    // Header bar.
    o << "<header><div class=\"wrap\">\n";
    o << "<h1><span class=\"accent\">dsd-server</span> status</h1>\n";
    o << "<div class=\"sub\">up " << html_escape(human_duration(s.uptime_s))
      << " &middot; since " << html_escape(format_utc(s.started))
      << " &middot; refreshes every " << kStatusRefreshSeconds << "s</div>\n";
    o << "</div></header>\n";

    o << "<div class=\"wrap\">\n";

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
        o << "<div class=\"section-label\">Active by protocol</div>\n<div style=\"margin-bottom:1.5rem\">";
        for (const auto& kv : s.by_protocol) {
            o << "<span class=\"tag\">" << html_escape(kv.first) << " <b>"
              << kv.second << "</b></span>";
        }
        o << "</div>\n";
    }

    // Session table.
    o << "<div class=\"section-label\">Sessions</div>\n";
    o << "<div class=\"panel\">\n<table>\n<thead><tr>"
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
              << "<td class=\"mono\">" << html_escape(r.remote.empty() ? "-" : r.remote) << "</td>"
              << "<td>" << html_escape(r.protocol) << "</td>"
              << "<td>" << html_escape(r.chain.empty() ? "-" : r.chain) << "</td>"
              << "<td class=\"" << (r.active ? "state-on" : "state-off") << "\">"
              << "<span class=\"dot " << (r.active ? "on" : "off") << "\"></span>"
              << (r.active ? "decoding" : "idle") << "</td>"
              << "<td class=\"mono\">" << html_escape(format_utc(r.connected)) << "</td>"
              << "<td class=\"num\">" << html_escape(human_duration(dur)) << "</td>"
              << "</tr>\n";
        }
    }
    o << "</tbody>\n</table>\n</div>\n";      // .panel
    o << "</div>\n</body>\n</html>\n";        // .wrap
    return o.str();
}

} // namespace dsdsrv
