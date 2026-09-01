// tetrapol_fake_dump.cpp
//
// A tiny stand-in for tetrapol-kit's `tetrapol_dump`, used by
// test_tetrapol_process.cpp to exercise TetrapolProcess's subprocess + stdout
// parse path without the real decoder (the same idea as tetra_fake_rx). On
// start it prints two decoded TSDU trees to stdout in tetrapol_dump's exact
// multi-line indented format -- one D_SYSTEM_INFO broadcast (kind "sync") and
// one D_GROUP_ACTIVATION (kind "call", carrying GROUP_ID) -- then drains stdin
// (the demodulated bits) until EOF so the parent controls its lifetime exactly
// as it would the real child (stop() closes stdin -> EOF -> we exit).
//
// The trailing (headerless) message is emitted only when the parser sees the
// NEXT header or EOF, so printing two messages here also checks that the first
// is finalized when the second's header arrives and the second on flush().

#include <unistd.h>
#include <cstdio>

int main() {
    // D_SYSTEM_INFO -- pinned to tetrapol-kit lib/tsdu.c printf strings.
    std::printf("\tCODOP=0x90 (D_SYSTEM_INFO)\n");
    std::printf("\tPRIO=0\n");
    std::printf("\tID_TSAP=0\n");
    std::printf("\t\tCOUNTRY_CODE=1\n");
    std::printf("\t\tSYSTEM_ID\n");
    std::printf("\t\t\tVERSION=1\n");
    std::printf("\t\t\tNETWORK=42\n");
    std::printf("\t\tLOC_AREA_ID\n");
    std::printf("\t\t\tLOC_ID=7\n");
    std::printf("\t\tCELL_ID: BS_ID=12 RWS_ID=3\n");
    std::printf("\t\tCELL_BN=5\n");

    // D_GROUP_ACTIVATION -- a call event carrying a GROUP_ID (talkgroup).
    std::printf("\tCODOP=0x21 (D_GROUP_ACTIVATION)\n");
    std::printf("\tPRIO=2\n");
    std::printf("\tID_TSAP=1\n");
    std::printf("\t\tGROUP_ID=1234\n");

    // A trailing broadcast header. On a continuous downlink each message is
    // finalized when the NEXT CODOP header arrives, so this makes the
    // D_GROUP_ACTIVATION above emit promptly (as it would live) rather than
    // only at EOF -- the parser's flush() covers this last one at stop().
    std::printf("\tCODOP=0x90 (D_SYSTEM_INFO)\n");
    std::printf("\t\tNETWORK=42\n");
    std::fflush(stdout);

    // Drain stdin until EOF (parent closed its write end in stop()).
    char buf[4096];
    while (::read(STDIN_FILENO, buf, sizeof(buf)) > 0) { /* discard bits */ }
    return 0;
}
