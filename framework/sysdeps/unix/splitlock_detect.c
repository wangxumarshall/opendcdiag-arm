/*
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <signal.h>
#include <stdalign.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __x86_64__
static void splitlock_signal_handler(int signum)
{
    _exit(signum);
}

static __attribute__((noreturn)) void splitlock_detection()
{
    alignas(64) char buffer[64 + sizeof(int)];
    int *misaligned = (int*)&buffer[64 - 2];
    int v = 1;

    signal(SIGSEGV, splitlock_signal_handler);
    signal(SIGBUS, splitlock_signal_handler);

    __asm__ volatile ("lock xchg %1, %0" : "=m" (*misaligned), "+r" (v));
    _exit(0);
}

bool splitlock_enforcement_enabled()
{
    static int cached_result = 0;   /* positive: enabled; negative: disabled */
    if (cached_result)
        return cached_result > 0;

    pid_t child = vfork();
    if (child == 0)
        splitlock_detection();             /* child process */
    if (child < 0)
        return false;

    /* parent, wait for child */
    int status;
    waitpid(child, &status, 0);

    /* if the child crashed or exited with a status different from 0,
     * split locks are prohibited */
    bool enforced = !WIFEXITED(status) || WEXITSTATUS(status) != 0;
    cached_result = enforced ? 1 : -1;
    return enforced;
}
#else
/* On AArch64 (and other non-x86), split-lock enforcement isn't a
 * configurable kernel feature like x86's #AC exception. AArch64 atomics
 * are *architecturally* required to be aligned: an atomic operation on an
 * unaligned address unconditionally faults (SIGBUS), there is no "lenient"
 * mode to detect at runtime. So split-locked atomics are always
 * "enforced" (they always trap) on ARM, and there is nothing to probe.
 *
 * (splitlock_enforcement_enabled() is currently unused by the framework,
 * but report the correct semantics rather than a stale false stub.) */
bool splitlock_enforcement_enabled()
{
    return true;
}
#endif
