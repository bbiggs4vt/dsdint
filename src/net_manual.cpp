// net_manual.cpp -- see net_manual.hpp.
//
// The PDF is pulled in byte for byte by the assembler (.incbin) at the path
// CMake passes as DSD_NET_MANUAL_PDF; CMake also makes this file rebuild when
// the PDF changes (OBJECT_DEPENDS).

#include "net_manual.hpp"

#include <cstddef>

#ifdef DSD_NET_MANUAL_PDF
asm(".section .rodata\n"
    ".balign 16\n"
    ".global dsd_net_manual_pdf_begin\n"
    ".type dsd_net_manual_pdf_begin, @object\n"
    "dsd_net_manual_pdf_begin:\n"
    ".incbin \"" DSD_NET_MANUAL_PDF "\"\n"
    ".global dsd_net_manual_pdf_end\n"
    ".type dsd_net_manual_pdf_end, @object\n"
    "dsd_net_manual_pdf_end:\n"
    ".previous\n");
extern "C" const char dsd_net_manual_pdf_begin[];
extern "C" const char dsd_net_manual_pdf_end[];
#endif

namespace dsdsrv {

std::string_view net_manual_pdf() {
#ifdef DSD_NET_MANUAL_PDF
    return std::string_view(dsd_net_manual_pdf_begin,
                            static_cast<std::size_t>(dsd_net_manual_pdf_end - dsd_net_manual_pdf_begin));
#else
    return std::string_view();
#endif
}

} // namespace dsdsrv
