/*
 * The Shadow Simulator
 * See LICENSE for licensing information
 */

#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "main/core/support/object_counter.h"
#include "main/core/worker.h"
#include "main/host/descriptor/descriptor.h"
#include "main/host/descriptor/timer.h"
#include "main/host/process.h"
#include "main/host/syscall/epoll.h"
#include "main/host/syscall/protected.h"
#include "main/host/syscall/socket.h"
#include "main/host/syscall/time.h"
#include "main/host/syscall/unistd.h"
#include "main/host/syscall_handler.h"
#include "main/host/syscall_types.h"
#include "main/host/thread.h"
#include "support/logger/logger.h"

SysCallHandler* syscallhandler_new(Host* host, Process* process,
                                   Thread* thread) {
    utility_assert(host);
    utility_assert(process);
    utility_assert(thread);

    SysCallHandler* sys = malloc(sizeof(SysCallHandler));

    *sys = (SysCallHandler){
        .host = host,
        .process = process,
        .thread = thread,
        .blockedSyscallNR = -1,
        .referenceCount = 1,
        /* Here we create the timer directly rather than going
         * through host_createDescriptor because the descriptor
         * is not being used to service a plugin syscall and it
         * should not be tracked with an fd handle. */
        .timer = timer_new(0, CLOCK_MONOTONIC, 0),
    };

    MAGIC_INIT(sys);

    host_ref(host);
    process_ref(process);
    thread_ref(thread);

    worker_countObject(OBJECT_TYPE_SYSCALL_HANDLER, COUNTER_TYPE_NEW);
    return sys;
}

static void _syscallhandler_free(SysCallHandler* sys) {
    MAGIC_ASSERT(sys);

    if (sys->host) {
        host_unref(sys->host);
    }
    if (sys->process) {
        process_unref(sys->process);
    }
    if (sys->thread) {
        thread_unref(sys->thread);
    }

    if (sys->timer) {
        descriptor_unref(sys->timer);
    }

    MAGIC_CLEAR(sys);
    free(sys);
    worker_countObject(OBJECT_TYPE_SYSCALL_HANDLER, COUNTER_TYPE_FREE);
}

void syscallhandler_ref(SysCallHandler* sys) {
    MAGIC_ASSERT(sys);
    (sys->referenceCount)++;
}

void syscallhandler_unref(SysCallHandler* sys) {
    MAGIC_ASSERT(sys);
    (sys->referenceCount)--;
    utility_assert(sys->referenceCount >= 0);
    if(sys->referenceCount == 0) {
        _syscallhandler_free(sys);
    }
}

static void _syscallhandler_pre_syscall(SysCallHandler* sys, long number,
                                        const char* name) {
    debug("SYSCALL_HANDLER_PRE(%s,pid=%u): handling syscall %ld %s%s",
          process_getPluginName(sys->process),
          process_getProcessID(sys->process), number, name,
          _syscallhandler_wasBlocked(sys) ? " (previously BLOCKed)" : "");
}

static void _syscallhandler_post_syscall(SysCallHandler* sys, long number,
                                         const char* name, SysCallReturn* scr) {
    debug("SYSCALL_HANDLER_POST(%s,pid=%u): syscall %ld %s result: state=%s%s "
          "code=%d",
          process_getPluginName(sys->process),
          process_getProcessID(sys->process), number, name,
          _syscallhandler_wasBlocked(sys) ? "BLOCK->" : "",
          scr->state == SYSCALL_DONE
              ? "DONE"
              : scr->state == SYSCALL_BLOCK
                    ? "BLOCK"
                    : scr->state == SYSCALL_NATIVE ? "NATIVE" : "UNKNOWN",
          (int)scr->retval.as_i64);
}

///////////////////////////////////////////////////////////
// Single public API function for calling Shadow syscalls
///////////////////////////////////////////////////////////

#define HANDLE(s)                                                              \
    case SYS_##s:                                                              \
        _syscallhandler_pre_syscall(sys, args->number, #s);                    \
        scr = syscallhandler_##s(sys, args);                                   \
        _syscallhandler_post_syscall(sys, args->number, #s, &scr);             \
        break
#define NATIVE(s)                                                              \
    case SYS_##s:                                                              \
        debug("native syscall %ld " #s, args->number);                         \
        scr = (SysCallReturn){.state = SYSCALL_NATIVE};                        \
        break
SysCallReturn syscallhandler_make_syscall(SysCallHandler* sys,
                                          const SysCallArgs* args) {
    MAGIC_ASSERT(sys);

    SysCallReturn scr;

    /* Make sure that we either don't have a blocked syscall,
     * or if we blocked a syscall, then that same syscall
     * should be executed again when it becomes unblocked. */
    if (sys->blockedSyscallNR >= 0 && sys->blockedSyscallNR != args->number) {
        error("We blocked syscall number %ld but syscall number %ld "
              "is unexpectedly being invoked",
              sys->blockedSyscallNR, args->number);
    }

    switch (args->number) {
        HANDLE(accept);
        HANDLE(accept4);
        HANDLE(bind);
        HANDLE(clock_gettime);
        HANDLE(close);
        HANDLE(connect);
        HANDLE(epoll_create);
        HANDLE(epoll_create1);
        HANDLE(epoll_ctl);
        HANDLE(epoll_wait);
        HANDLE(getpeername);
        HANDLE(getpid);
        HANDLE(getsockname);
        HANDLE(listen);
        HANDLE(nanosleep);
        HANDLE(pipe);
        HANDLE(pipe2);
        HANDLE(read);
        HANDLE(recvfrom);
        HANDLE(sendto);
        HANDLE(shutdown);
        HANDLE(socket);
        HANDLE(uname);
        HANDLE(write);

        // **************************************
        // Needed for phold, but not handled yet:
        // **************************************
        // Test coverage: test/file
        NATIVE(fstat);
        // Test coverage: test/file (via open(3))
        NATIVE(openat);

        // **************************************
        // Not handled (yet):
        // **************************************
        NATIVE(access);
        NATIVE(arch_prctl);
        NATIVE(brk);
        NATIVE(execve);
        NATIVE(mmap);
        NATIVE(mprotect);
        NATIVE(munmap);
        NATIVE(prlimit64);
        NATIVE(rt_sigaction);
        NATIVE(rt_sigprocmask);
        NATIVE(set_robust_list);
        NATIVE(set_tid_address);
        NATIVE(stat);
        default:
            info("unhandled syscall %ld", args->number);
            scr = (SysCallReturn){.state = SYSCALL_NATIVE};
            break;
    }

    if (scr.state == SYSCALL_BLOCK) {
        /* We are blocking: store the syscall number so we know
         * to expect the same syscall again when it unblocks. */
        sys->blockedSyscallNR = args->number;
    } else if (_syscallhandler_wasBlocked(sys)) {
        /* We were but are no longer blocked on a syscall. Make
         * sure any previously used listener timeouts are ignored.*/
        _syscallhandler_setListenTimeout(sys, NULL);
        sys->blockedSyscallNR = -1;
    }

    return scr;
}
#undef NATIVE
#undef HANDLE