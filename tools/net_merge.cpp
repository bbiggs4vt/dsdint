// net_merge.cpp -- merge network-explorer exports into one.
//
//   net-merge <export.json[.gz]> [more exports...] -o <merged.json> [--graphml <merged.graphml>]
//
// The same merge as the explorer's "Open..." with several files (and its
// "Import..." into a live server): talkgroups, radios and strong network ids
// (P25 WACN/SysID, DMR network id, ...) join across files; weak network ids
// (a bare color code / NAC / RAN) stay apart per server run. A file whose data
// is already in the merge (the same export twice, or an older export of a run
// a newer file covers) is skipped or replaced instead of counted twice; one
// that only partly overlaps another is refused. Prints what happened to each
// input and a summary per protocol. The result opens in the explorer
// ("Open...") and can be merged again.
//
// Exit status: 0 = merged, 1 = usage / I/O error, 2 = some input not merged
// (invalid, or refused).

#include "assoc_merge.hpp"

#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

using namespace dsdsrv;

namespace {

bool read_file(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}
bool write_file(const std::string& path, const std::string& data) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << data;
    return static_cast<bool>(f);
}
std::string base(const std::string& p) {
    const std::size_t s = p.find_last_of('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> inputs;
    std::string out, gml;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if ((a == "-o" || a == "--out") && i + 1 < argc) out = argv[++i];
        else if (a == "--graphml" && i + 1 < argc) gml = argv[++i];
        else if (a == "-h" || a == "--help") { inputs.clear(); out.clear(); break; }
        else inputs.push_back(a);
    }
    if (inputs.empty() || (out.empty() && gml.empty())) {
        std::cerr << "usage: net-merge <export.json[.gz]> [more...] -o <merged.json> [--graphml <merged.graphml>]\n";
        return 1;
    }
    std::vector<std::pair<std::string, std::string>> files;
    for (const auto& p : inputs) {
        std::string text;
        if (!read_file(p, text)) { std::cerr << "net-merge: cannot read " << p << "\n"; return 1; }
        files.emplace_back(base(p), std::move(text));
    }
    std::vector<MergeReport> report;
    const Dataset d = merge_exports(files, report);
    bool all = true;
    for (const auto& r : report) {
        std::cout << "  " << r.label << ": " << r.status << (r.message.empty() || r.message == r.status ? "" : " -- " + r.message) << "\n";
        all = all && (r.status == "merged" || r.status == "replaced" || r.status == "skipped");
    }
    std::cout << "sources:";
    for (const auto& s : d.sources) std::cout << "\n  " << span_text(s) << "  (" << s.instance << ")";
    std::cout << "\n";
    for (const auto& fk : d.fams) {
        const DsFamily& F = fk.second;
        std::cout << fk.first << ": " << F.networks.size() << " networks, " << F.tgs.size() << " talkgroups, "
                  << F.radios.size() << " radios, " << F.calls.size() << " calls\n";
    }
    const std::int64_t now = d.exported;
    if (!out.empty() && !write_file(out, export_json(d, now, "", "merged"))) {
        std::cerr << "net-merge: cannot write " << out << "\n";
        return 1;
    }
    if (!gml.empty() && !write_file(gml, graphml(d, now))) {
        std::cerr << "net-merge: cannot write " << gml << "\n";
        return 1;
    }
    return all ? 0 : 2;
}
