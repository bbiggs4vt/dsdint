// child_fds.hpp
//
// Used by every decoder subprocess backend (DsdProcess, TetraProcess,
// TetraKitProcess, MultimonProcess) in the child between fork() and exec().
//
// Our own pipes/sockets are opened O_CLOEXEC, but Asio's sockets are NOT
// close-on-exec. Without this, every decoder child inherits a copy of the
// listening socket and of every WebSocket connection open at the moment it
// was forked. Those TCP sockets then outlive the sessions that owned them
// (lingering in CLOSE_WAIT inside some unrelated session's decoder) until
// that decoder exits. tests/test_session.cpp's
// test_decoder_children_inherit_no_sockets pins this for each backend.

#pragma once

#include <sys/syscall.h>
#include <unistd.h>

namespace dsdsrv {

// Close every fd above stderr except `keep` (the exec-status pipe's write
// end, which CLOEXEC closes on a successful exec). Child side only: uses
// async-signal-safe calls exclusively.
inline void close_inherited_fds(int keep) {
#if defined(SYS_close_range)
    if (keep > 3) syscall(SYS_close_range, 3u, static_cast<unsigned>(keep - 1), 0u);
    if (syscall(SYS_close_range, static_cast<unsigned>(keep + 1), ~0u, 0u) == 0) return;
#endif
    // Kernel without close_range (< 5.9): close them one by one.
    long max_fd = sysconf(_SC_OPEN_MAX);
    if (max_fd < 0 || max_fd > 65536) max_fd = 65536;
    for (int fd = 3; fd < max_fd; ++fd)
        if (fd != keep) close(fd);
}

} // namespace dsdsrv
