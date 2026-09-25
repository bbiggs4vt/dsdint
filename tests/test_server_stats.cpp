// test_server_stats.cpp
//
// Unit tests for the status-page backend: ServerStats' registry logic
// (src/server_stats.hpp) and the HTML/JSON renderers (src/status_page.hpp).
// Pure in-process logic -- no sockets, no server, always runs.

#include "../src/server_stats.hpp"
#include "../src/status_page.hpp"

#include <cstdio>
#include <string>

using namespace dsdsrv;

static int g_failures = 0;
static void check(bool c, const char* what) {
    std::printf("  %s: %s\n", c ? "OK" : "FAIL", what);
    if (!c) ++g_failures;
}

static bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

int main() {
    std::printf("test_server_stats\n");

    // ---- registry: add / protocol / active / remove ----
    {
        ServerStats st;
        auto s0 = st.snapshot();
        check(s0.total_sessions == 0 && s0.current_sessions == 0 && s0.active_pipelines == 0,
              "empty registry: all counters zero");
        check(s0.rows.empty() && s0.by_protocol.empty(), "empty registry: no rows");

        auto a = st.add_session("10.0.0.1:1000");
        auto b = st.add_session("10.0.0.2:2000");
        check(a == 1 && b == 2, "ids are assigned 1,2 in order");

        auto s1 = st.snapshot();
        check(s1.total_sessions == 2 && s1.current_sessions == 2, "two connected, two total");
        check(s1.active_pipelines == 0, "no pipeline active before start");
        check(s1.rows.size() == 2 && s1.rows[0].id == 1 && s1.rows[1].id == 2,
              "rows sorted by id");
        check(s1.rows[0].protocol == "-" && s1.rows[0].chain.empty() && !s1.rows[0].active,
              "fresh session: protocol '-', no chain, idle");
        check(s1.rows[0].remote == "10.0.0.1:1000", "remote address recorded");

        st.set_protocol(a, "dmr", "fm", true);
        auto s2 = st.snapshot();
        check(s2.active_pipelines == 1, "one active pipeline after start");
        check(s2.by_protocol.size() == 1 && s2.by_protocol.at("dmr") == 1,
              "by_protocol counts the active dmr session");
        check(s2.rows[0].protocol == "dmr" && s2.rows[0].chain == "fm" && s2.rows[0].active,
              "row reflects protocol/chain/active");

        // second session on tetra, then both active -> two protocols
        st.set_protocol(b, "tetra", "tetra", true);
        auto s3 = st.snapshot();
        check(s3.active_pipelines == 2 && s3.by_protocol.size() == 2 &&
              s3.by_protocol.at("tetra") == 1, "both active, two protocols");

        // stop the first: idle but keeps its label
        st.set_pipeline_active(a, false);
        auto s4 = st.snapshot();
        check(s4.active_pipelines == 1, "one active after stopping the other");
        check(!s4.rows[0].active && s4.rows[0].protocol == "dmr",
              "stopped session goes idle but keeps its protocol label");
        check(s4.by_protocol.count("dmr") == 0 && s4.by_protocol.at("tetra") == 1,
              "by_protocol only counts still-active pipelines");

        // disconnect: current drops, total stays, and it lands in history
        st.remove_session(a);
        auto s5 = st.snapshot();
        check(s5.current_sessions == 1 && s5.total_sessions == 2,
              "remove drops current, total is cumulative");
        check(s5.rows.size() == 1 && s5.rows[0].id == 2, "only the remaining row is listed");
        check(s5.history.size() == 1 && s5.history[0].id == 1,
              "removed session is archived to history");
        check(s5.history[0].protocol == "dmr", "history keeps the session's last protocol");

        // disconnect the second: history now has both, newest first
        st.remove_session(b);
        auto s6 = st.snapshot();
        check(s6.current_sessions == 0, "both disconnected");
        check(s6.history.size() == 2 && s6.history[0].id == 2 && s6.history[1].id == 1,
              "history is newest-first");

        // ops on an unknown id are no-ops, never crash
        st.set_protocol(999, "dmr", "fm", true);
        st.set_pipeline_active(999, false);
        st.remove_session(999);
        check(st.snapshot().current_sessions == 0, "unknown-id ops are harmless no-ops");
    }

    // ---- history is bounded to the configured limit ----
    {
        ServerStats st(3);   // keep only the last 3 finished sessions
        for (int i = 0; i < 6; ++i) st.remove_session(st.add_session("h:" + std::to_string(i)));
        auto s = st.snapshot();
        check(s.history.size() == 3, "history caps at the configured limit (3)");
        check(s.total_sessions == 6, "total still counts every session ever");
        // newest-first: the last three added were ids 4,5,6 -> 6,5,4
        check(s.history[0].id == 6 && s.history[2].id == 4,
              "capped history keeps the most recent, newest-first");
    }

    // ---- log buffer: capture, newest-first, cap, count ----
    {
        ServerStats st(50, /*log_limit=*/3);
        st.add_log(1, "{\"type\":\"started\"}");
        st.add_log(1, "{\"type\":\"event\",\"kind\":\"call\"}");
        auto snap = st.log_snapshot();
        check(snap.size() == 2, "log captures added frames");
        check(snap[0].text == "{\"type\":\"event\",\"kind\":\"call\"}",
              "log_snapshot is newest-first");
        check(snap[0].session_id == 1, "log entry keeps its session id");
        check(st.snapshot().log_lines == 2, "snapshot reports the log line count");

        // exceed the cap -> oldest dropped
        st.add_log(2, "a"); st.add_log(2, "b");
        auto snap2 = st.log_snapshot();
        check(snap2.size() == 3, "log is bounded to its limit (3)");
        check(snap2[0].text == "b" && snap2[2].text == "{\"type\":\"event\",\"kind\":\"call\"}",
              "cap drops the oldest, keeps newest-first order");

        std::string lj = render_log_json(st.log_snapshot());
        check(contains(lj, "\"log\":["), "log json has a log array");
        check(contains(lj, "\"session\":2"), "log json carries the session id");
        // The frame text is embedded as an escaped JSON string.
        check(contains(lj, "\\\"type\\\":\\\"event\\\""), "log json escapes the frame text");
    }
    {
        // A zero limit disables logging entirely.
        ServerStats st(50, 0);
        st.add_log(1, "x");
        check(st.log_snapshot().empty() && st.snapshot().log_lines == 0,
              "log_limit 0 disables the log buffer");
    }

    // ---- JSON renderer: shape + escaping ----
    {
        ServerStats st;
        auto id = st.add_session("1.2.3.4:5");
        st.set_protocol(id, "dmr", "fm", true);
        std::string j = render_status_json(st.snapshot());
        check(contains(j, "\"total_sessions\":1"), "json has total_sessions");
        check(contains(j, "\"current_sessions\":1"), "json has current_sessions");
        check(contains(j, "\"active_pipelines\":1"), "json has active_pipelines");
        check(contains(j, "\"by_protocol\":{\"dmr\":1}"), "json has by_protocol map");
        check(contains(j, "\"sessions\":["), "json has sessions array");
        check(contains(j, "\"protocol\":\"dmr\""), "json session carries protocol");
        check(contains(j, "\"chain\":\"fm\""), "json session carries chain");
        check(contains(j, "\"active\":true"), "json session active flag is a bare bool");
        check(contains(j, "\"remote\":\"1.2.3.4:5\""), "json session carries remote");
        check(contains(j, "\"log_lines\":"), "json reports the log line count");
        check(contains(j, "\"history\":[]"), "json has an (empty) history array");
        st.remove_session(id);
        std::string j2 = render_status_json(st.snapshot());
        check(contains(j2, "\"history\":[{"), "json history carries the finished session");
        check(contains(j2, "\"ended\":\""), "json history row has an ended timestamp");
    }
    {
        // A remote string with JSON-special characters must be escaped so the
        // output stays valid JSON.
        ServerStats st;
        st.add_session("a\"b\\c");
        std::string j = render_status_json(st.snapshot());
        check(contains(j, "\"remote\":\"a\\\"b\\\\c\""), "json escapes quotes/backslashes");
    }

    // ---- HTML renderer: contains the counts and escapes markup ----
    {
        ServerStats st;
        auto id = st.add_session("<script>:80");   // hostile-looking remote
        st.set_protocol(id, "dmr", "fm", true);
        std::string h = render_status_html(st.snapshot());
        check(contains(h, "<!DOCTYPE html>"), "html is a full document");
        check(contains(h, "http-equiv=\"refresh\""), "html auto-refreshes");
        check(contains(h, "Current sessions"), "html labels current sessions");
        check(contains(h, "Total sessions"), "html labels total sessions");
        check(contains(h, "&lt;script&gt;:80"), "html escapes the remote address");
        check(!contains(h, "<script>:80"), "raw markup does not leak into html");
        check(contains(h, "data-tab=\"tab-sessions\"") && contains(h, "data-tab=\"tab-history\"") &&
              contains(h, "data-tab=\"tab-log\""),
              "html has Sessions, History and Log tabs");
        check(contains(h, "no finished sessions yet"), "empty history shows a placeholder row");
        st.remove_session(id);
        std::string h2 = render_status_html(st.snapshot());
        check(contains(h2, "&lt;script&gt;:80") && !contains(h2, "<script>:80"),
              "history table also escapes the remote address");
        check(contains(h2, "Ended (UTC)"), "history table has an Ended column");
    }

    // ---- helpers ----
    {
        using namespace status_detail;
        check(human_duration(0) == "0s", "duration 0s");
        check(human_duration(59) == "59s", "duration 59s");
        check(human_duration(60) == "1m 0s", "duration 1m 0s");
        check(human_duration(3661) == "1h 1m 1s", "duration 1h 1m 1s");
        check(html_escape("a<b>&\"'") == "a&lt;b&gt;&amp;&quot;&#39;", "html_escape all specials");
        check(json_escape(std::string("x\ny")) == "x\\ny", "json_escape newline");
    }

    if (g_failures == 0) {
        std::printf("\nALL SERVER STATS TESTS PASSED\n");
        return 0;
    }
    std::printf("\n%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
