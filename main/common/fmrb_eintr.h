#pragma once

// EINTR handling for the Linux simulation build.
//
// The FreeRTOS POSIX port drives its tick with SIGALRM (1 kHz) and installs the
// handler without SA_RESTART. The tick is always delivered to the thread of the
// running task, so any syscall that sleeps in the kernel -- even a bind() on a
// Unix socket -- can come back with EINTR instead of being restarted. Every
// syscall on the Linux paths that can sleep is therefore retried here, the same
// way display_shm.cpp already does for shm_open / sem_open.

#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

// Evaluate a syscall expression until it does not fail with EINTR.
#define FMRB_RETRY_EINTR(expr)                                   \
    ({                                                           \
        __typeof__(expr) fmrb_eintr_r_;                          \
        do {                                                     \
            fmrb_eintr_r_ = (expr);                              \
        } while (fmrb_eintr_r_ == -1 && errno == EINTR);         \
        fmrb_eintr_r_;                                           \
    })

// bind() a Unix socket to addr->sun_path, retrying on EINTR. An interrupted
// bind may leave the socket node behind, which would turn the retry into
// EADDRINUSE, so the node is removed before each retry (the callers own the
// path and unlink it before the first attempt anyway).
static inline int fmrb_unix_bind(int fd, const struct sockaddr_un *addr)
{
    int r;
    while ((r = bind(fd, (const struct sockaddr *)addr, sizeof(*addr))) < 0 &&
           errno == EINTR) {
        unlink(addr->sun_path);
    }
    return r;
}
