// net_manual.hpp
//
// The network explorer's user manual (docs/NET_EXPLORER.pdf), built into the
// server so the explorer's Help (/net/manual.pdf) works wherever the binary
// goes -- no docs folder to install. Empty when the build had no PDF.

#pragma once

#include <string_view>

namespace dsdsrv {

std::string_view net_manual_pdf();

} // namespace dsdsrv
