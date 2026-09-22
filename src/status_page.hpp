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

// With JavaScript on, the page live-polls /status.json this often and
// patches the DOM in place (no flicker, tiny payload). With JS off, a
// <noscript> <meta refresh> falls back to a full reload at the slower
// kStatusRefreshSeconds cadence.
inline constexpr int kStatusPollMs = 1000;
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
      // JS-off fallback only: browsers with JS run the in-place poller below.
      << "<noscript><meta http-equiv=\"refresh\" content=\"" << kStatusRefreshSeconds
      << "\"></noscript>\n"
      << "<title>dsd-server status</title>\n"
      << "<style>\n"
      // Bootswatch Slate palette (self-contained -- no CDN, so the page
      // works on an isolated/offline server). Values mirror Slate's SCSS:
      // body #272b30, panels/inputs #3a3f44, muted #7a8288, borders
      // rgba(0,0,0,.6)/#1c1e22, success #62c462, info #5bc0de.
      << "  :root {\n"
      << "    --bg: #272b30; --panel: #3a3f44; --text: #c8c8c8; --heading: #fff;\n"
      << "    --muted: #7a8288; --primary: #7a8288; --success: #62c462; --info: #5bc0de;\n"
      << "    --comp-bd: rgba(0,0,0,.6); --table-bd: #1c1e22;\n"
      << "    color-scheme: dark;\n"
      << "  }\n"
      << "  * { box-sizing: border-box; }\n"
      << "  html, body { background: var(--bg); }\n"
      << "  body { color: var(--text); margin: 0; padding: 0 0 2rem;\n"
      << "         font-family: 'Helvetica Neue', Helvetica, Arial, sans-serif;\n"
      << "         font-size: 15px; line-height: 1.42857; }\n"
      << "  .wrap { max-width: 1100px; margin: 0 auto; padding: 0 1.25rem; }\n"
      // Slate navbar: metallic vertical gradient + dark 1px border.
      << "  header { background-image: linear-gradient(#484e55, #3a3f44 60%, #2e3236);\n"
      << "           border-bottom: 1px solid var(--comp-bd);\n"
      << "           box-shadow: inset 0 1px 0 rgba(255,255,255,.08); margin-bottom: 1.5rem; }\n"
      << "  header .wrap { padding-top: 0.9rem; padding-bottom: 0.9rem; }\n"
      << "  h1 { font-size: 1.4rem; margin: 0; font-weight: 500; color: var(--heading);\n"
      << "       text-shadow: 0 -1px 0 rgba(0,0,0,.4); }\n"
      << "  h1 .accent { color: var(--info); }\n"
      << "  .sub { color: var(--muted); font-size: 0.82rem; margin-top: 0.25rem; }\n"
      << "  .cards { display: flex; flex-wrap: wrap; gap: 0.9rem; margin-bottom: 1.5rem; }\n"
      // Slate card/well: panel gray, metallic top highlight, dark border.
      << "  .card { background-image: linear-gradient(#3e444a, #3a3f44 60%, #363b40);\n"
      << "          border: 1px solid var(--comp-bd); border-radius: 4px;\n"
      << "          box-shadow: inset 0 1px 0 rgba(255,255,255,.06); padding: 0.85rem 1.15rem;\n"
      << "          min-width: 9rem; }\n"
      << "  .card .n { font-size: 2rem; font-weight: 500; color: var(--heading);\n"
      << "             font-variant-numeric: tabular-nums; text-shadow: 0 -1px 0 rgba(0,0,0,.3); }\n"
      << "  .card .l { color: var(--muted); font-size: 0.72rem; text-transform: uppercase;\n"
      << "             letter-spacing: 0.06em; margin-top: 0.15rem; }\n"
      << "  .panel { background: var(--panel); border: 1px solid var(--comp-bd);\n"
      << "           border-radius: 4px; overflow: hidden;\n"
      << "           box-shadow: inset 0 1px 0 rgba(255,255,255,.05); }\n"
      << "  table { border-collapse: collapse; width: 100%; font-size: 0.9rem; }\n"
      << "  th, td { text-align: left; padding: 0.5rem 0.85rem; }\n"
      // Slate table header: metallic strip, dark divider under it.
      << "  thead th { background-image: linear-gradient(#41474d, #3a3f44);\n"
      << "             color: var(--heading); font-weight: 500; font-size: 0.72rem;\n"
      << "             text-transform: uppercase; letter-spacing: 0.05em;\n"
      << "             border-bottom: 2px solid var(--table-bd); }\n"
      << "  tbody tr { border-top: 1px solid var(--table-bd); }\n"
      // Slate striping: translucent white overlay on the dark base.
      << "  tbody tr:nth-child(even) { background: rgba(255,255,255,.035); }\n"
      << "  tbody tr:hover { background: rgba(255,255,255,.075); }\n"
      << "  td.num { text-align: right; font-variant-numeric: tabular-nums; }\n"
      << "  td.mono { font-family: Menlo, Monaco, Consolas, 'Courier New', monospace;\n"
      << "            font-size: 0.85rem; color: var(--text); }\n"
      << "  .dot { display: inline-block; width: 0.55rem; height: 0.55rem; border-radius: 50%;\n"
      << "         margin-right: 0.4rem; vertical-align: baseline; }\n"
      << "  .on { background: var(--success); box-shadow: 0 0 6px rgba(98,196,98,.6); }\n"
      << "  .off { background: var(--muted); }\n"
      << "  .state-on { color: var(--success); }\n"
      << "  .state-off { color: var(--muted); }\n"
      << "  .empty { color: var(--muted); padding: 1rem 0.85rem; }\n"
      // Slate badge/button: gray gradient, white text, dark border.
      << "  .tag { display: inline-block; color: var(--heading);\n"
      << "         background-image: linear-gradient(rgba(255,255,255,.12), rgba(255,255,255,0)),\n"
      << "                           linear-gradient(#7a8288, #7a8288);\n"
      << "         border: 1px solid var(--comp-bd); border-radius: 4px;\n"
      << "         padding: 0.1rem 0.6rem; margin-right: 0.4rem; font-size: 0.8rem;\n"
      << "         text-shadow: 0 -1px 0 rgba(0,0,0,.3); font-variant-numeric: tabular-nums; }\n"
      << "  .tag b { font-weight: 700; }\n"
      << "  .section-label { color: var(--muted); font-size: 0.72rem; text-transform: uppercase;\n"
      << "                   letter-spacing: 0.06em; margin: 0 0 0.6rem; }\n"
      << "</style>\n</head>\n<body>\n";

    // Header bar.
    o << "<header><div class=\"wrap\">\n";
    o << "<h1><span class=\"accent\">dsd-server</span> status</h1>\n";
    o << "<div class=\"sub\">up <span id=\"uptime\">" << html_escape(human_duration(s.uptime_s))
      << "</span> &middot; since " << html_escape(format_utc(s.started))
      << " &middot; <span id=\"live\">live</span></div>\n";
    o << "</div></header>\n";

    o << "<div class=\"wrap\">\n";

    // Summary cards.
    o << "<div class=\"cards\">\n";
    auto card = [&](const std::string& id, const std::string& label, const std::string& value) {
        o << "  <div class=\"card\"><div class=\"n\" id=\"" << id << "\">" << html_escape(value)
          << "</div><div class=\"l\">" << html_escape(label) << "</div></div>\n";
    };
    card("cur", "Current sessions", std::to_string(s.current_sessions));
    card("tot", "Total sessions", std::to_string(s.total_sessions));
    card("act", "Active decodes", std::to_string(s.active_pipelines));
    o << "</div>\n";

    // Active-pipeline breakdown by protocol. The wrapper is always emitted
    // (hidden when empty) so the live poller has a stable node to fill.
    o << "<div id=\"byproto-sec\"" << (s.by_protocol.empty() ? " hidden" : "") << ">\n"
      << "<div class=\"section-label\">Active by protocol</div>\n"
      << "<div id=\"byproto\" style=\"margin-bottom:1.5rem\">";
    for (const auto& kv : s.by_protocol) {
        o << "<span class=\"tag\">" << html_escape(kv.first) << " <b>"
          << kv.second << "</b></span>";
    }
    o << "</div>\n</div>\n";

    // Session table.
    o << "<div class=\"section-label\">Sessions</div>\n";
    o << "<div class=\"panel\">\n<table>\n<thead><tr>"
      << "<th>#</th><th>Client</th><th>Protocol</th><th>Chain</th>"
      << "<th>State</th><th>Connected (UTC)</th><th class=\"num\">Duration</th>"
      << "</tr></thead>\n<tbody id=\"rows\">\n";
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
    o << "</div>\n";                          // .wrap

    // Live poller: fetch the JSON snapshot and patch the DOM in place, so
    // the page stays current without a flickering full-page reload. Nodes
    // are built with textContent, which escapes their values. If JS is off,
    // the <noscript> meta-refresh above reloads the whole page instead.
    o << "<script>\n(function(){\nvar POLL=" << kStatusPollMs << ";\n";
    o << R"JS(function dur(s){s=Math.max(0,Math.floor(s));var h=(s/3600)|0;s-=h*3600;var m=(s/60)|0;s-=m*60;var o='';if(h)o+=h+'h ';if(h||m)o+=m+'m ';return o+s+'s';}
function setText(id,v){var e=document.getElementById(id);if(e&&e.textContent!==v)e.textContent=v;}
function cell(cls,text){var td=document.createElement('td');if(cls)td.className=cls;td.textContent=text;return td;}
function render(d){
  setText('cur',''+d.current_sessions);
  setText('tot',''+d.total_sessions);
  setText('act',''+d.active_pipelines);
  setText('uptime',dur(d.uptime_seconds));
  var sec=document.getElementById('byproto-sec'),bp=document.getElementById('byproto');
  var keys=Object.keys(d.by_protocol||{}).sort();
  bp.textContent='';
  if(!keys.length){sec.hidden=true;}
  else{sec.hidden=false;keys.forEach(function(k){
    var sp=document.createElement('span');sp.className='tag';
    sp.appendChild(document.createTextNode(k+' '));
    var b=document.createElement('b');b.textContent=''+d.by_protocol[k];sp.appendChild(b);
    bp.appendChild(sp);});}
  var tb=document.getElementById('rows');tb.textContent='';
  var rows=d.sessions||[];
  if(!rows.length){var tr=document.createElement('tr');var td=cell('empty','no clients connected');td.colSpan=7;tr.appendChild(td);tb.appendChild(tr);return;}
  rows.forEach(function(r){
    var tr=document.createElement('tr');
    tr.appendChild(cell('num',''+r.id));
    tr.appendChild(cell('mono',r.remote||'-'));
    tr.appendChild(cell('',r.protocol));
    tr.appendChild(cell('',r.chain||'-'));
    var st=document.createElement('td');st.className=r.active?'state-on':'state-off';
    var dot=document.createElement('span');dot.className='dot '+(r.active?'on':'off');st.appendChild(dot);
    st.appendChild(document.createTextNode(r.active?'decoding':'idle'));tr.appendChild(st);
    tr.appendChild(cell('mono',r.connected));
    tr.appendChild(cell('num',dur(r.duration_seconds)));
    tb.appendChild(tr);});
}
function tick(){
  fetch('/status.json',{cache:'no-store'}).then(function(r){return r.json();})
    .then(render).catch(function(){})
    .then(function(){setTimeout(tick,POLL);});
}
setTimeout(tick,POLL);
})();
)JS";
    o << "</script>\n";

    o << "</body>\n</html>\n";
    return o.str();
}

} // namespace dsdsrv
