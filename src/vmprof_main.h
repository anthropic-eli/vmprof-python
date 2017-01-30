#pragma once

/* VMPROF
 *
 * statistical sampling profiler specifically designed to profile programs
 * which run on a Virtual Machine and/or bytecode interpreter, such as Python,
 * etc.
 *
 * The logic to dump the C stack traces is partly stolen from the code in
 * gperftools.
 * The file "getpc.h" has been entirely copied from gperftools.
 *
 * Tested only on gcc, linux, x86_64.
 *
 * Copyright (C) 2014-2015
 *   Antonio Cuni - anto.cuni@gmail.com
 *   Maciej Fijalkowski - fijall@gmail.com
 *   Armin Rigo - arigo@tunes.org
 *
 */

#define _GNU_SOURCE 1

#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>

#include "stack.h"
#include "vmprof_getpc.h"
#include "vmprof_mt.h"
#include "vmprof_common.h"
#include "compat.h"

#if defined(__unix__)
#include "rss_unix.h"
#elif defined(__APPLE__)
#include "rss_darwin.h"
#endif


/************************************************************/

static void *(*mainloop_get_virtual_ip)(char *) = 0;
static int opened_profile(const char *interp_name, int memory, int lines, int native);
static void flush_codes(void);

/************************************************************/

/* value: last bit is 1 if signals must be ignored; all other bits
   are a counter for how many threads are currently in a signal handler */
static long volatile signal_handler_value = 1;

RPY_EXTERN
void vmprof_ignore_signals(int ignored)
{
    if (!ignored) {
        __sync_fetch_and_and(&signal_handler_value, ~1L);
    }
    else {
        /* set the last bit, and wait until concurrently-running signal
           handlers finish */
        while (__sync_or_and_fetch(&signal_handler_value, 1L) != 1L) {
            usleep(1);
        }
    }
}


/* *************************************************************
 * functions to write a profile file compatible with gperftools
 * *************************************************************
 */

static char atfork_hook_installed = 0;


/* *************************************************************
 * functions to dump the stack trace
 * *************************************************************
 */

__attribute__((always_inline))
static int get_stack_trace(PyThreadState * current, void** result, int max_depth)
{
    PyFrameObject *frame;
    if (!current) {
        return 0;
    }
    frame = current->frame;
    // skip over
    // _sigtramp
    // sigprof_handler
    // vmp_walk_and_record_stack
    int d = vmp_walk_and_record_stack(frame, result, max_depth, 3);
    return d;
}



/* *************************************************************
 * the signal handler
 * *************************************************************
 */

#include <setjmp.h>

volatile int spinlock;
jmp_buf restore_point;

static void segfault_handler(int arg)
{
    longjmp(restore_point, SIGSEGV);
}

__attribute__((always_inline))
void _vmprof_sample_stack(struct profbuf_s *p, PyThreadState *tstate)
{
    int depth;
    struct prof_stacktrace_s *st = (struct prof_stacktrace_s *)p->data;
    st->marker = MARKER_STACKTRACE;
    st->count = 1;
    depth = get_stack_trace(tstate, st->stack, MAX_STACK_DEPTH-1);
    //st->stack[0] = GetPC((ucontext_t*)ucontext);
    // we gonna need that for pypy
    st->depth = depth;
    st->stack[depth++] = tstate;
    long rss = get_current_proc_rss();
    if (rss >= 0)
        st->stack[depth++] = (void*)rss;
    p->data_offset = offsetof(struct prof_stacktrace_s, marker);
    p->data_size = (depth * sizeof(void *) +
                    sizeof(struct prof_stacktrace_s) -
                    offsetof(struct prof_stacktrace_s, marker));
}

static void sigprof_handler(int sig_nr, siginfo_t* info, void *ucontext)
{
    PyThreadState * tstate = NULL;
    void (*prevhandler)(int);
    // TERRIBLE HACK AHEAD
    // on OS X, the thread local storage is sometimes uninitialized
    // when the signal handler runs - it means it's impossible to read errno
    // or call any syscall or read PyThread_Current or pthread_self. Additionally,
    // it seems impossible to read the register gs.
    // here we register segfault handler (all guarded by a spinlock) and call
    // longjmp in case segfault happens while reading a thread local
    //
    // We do the same error detection for linux to ensure that
    // get_current_thread_state returns a sane result
    while (__sync_lock_test_and_set(&spinlock, 1)) {
    }
    prevhandler = signal(SIGSEGV, &segfault_handler);
    int fault_code = setjmp(restore_point);
    if (fault_code == 0) {
        pthread_self();
        tstate = PyGILState_GetThisThreadState();
    } else {
        signal(SIGSEGV, prevhandler);
        __sync_lock_release(&spinlock);
        return;    
    }
    signal(SIGSEGV, prevhandler);
    __sync_lock_release(&spinlock);
    long val = __sync_fetch_and_add(&signal_handler_value, 2L);

    if ((val & 1) == 0) {
        int saved_errno = errno;
        int fd = vmp_profile_fileno();
        assert(fd >= 0);

        struct profbuf_s *p = reserve_buffer(fd);
        if (p == NULL) {
            /* ignore this signal: there are no free buffers right now */
        } else {
            _vmprof_sample_stack(p, tstate);
            commit_buffer(fd, p);
        }

        errno = saved_errno;
    }

    __sync_sub_and_fetch(&signal_handler_value, 2L);
}



/* *************************************************************
 * the setup and teardown functions
 * *************************************************************
 */

static int install_sigprof_handler(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = sigprof_handler;
    sa.sa_flags = SA_RESTART | SA_SIGINFO;
    if (sigemptyset(&sa.sa_mask) == -1 ||
        sigaction(SIGPROF, &sa, NULL) == -1)
        return -1;
    return 0;
}

static int remove_sigprof_handler(void)
{
    if (signal(SIGPROF, SIG_DFL) == SIG_ERR)
        return -1;
    return 0;
}

static int install_sigprof_timer(void)
{
    static struct itimerval timer;
    timer.it_interval.tv_sec = 0;
    timer.it_interval.tv_usec = profile_interval_usec;
    timer.it_value = timer.it_interval;
    if (setitimer(ITIMER_PROF, &timer, NULL) != 0)
        return -1;
    return 0;
}

static int remove_sigprof_timer(void) {
    static struct itimerval timer;
    timer.it_interval.tv_sec = 0;
    timer.it_interval.tv_usec = 0;
    timer.it_value.tv_sec = 0;
    timer.it_value.tv_usec = 0;
    if (setitimer(ITIMER_PROF, &timer, NULL) != 0)
        return -1;
    return 0;
}

static void atfork_disable_timer(void) {
    if (profile_interval_usec > 0) {
        remove_sigprof_timer();
        is_enabled = 0;
    }
}

static void atfork_enable_timer(void) {
    if (profile_interval_usec > 0) {
        install_sigprof_timer();
        is_enabled = 1;
    }
}

static void atfork_close_profile_file(void) {
    int fd = vmp_profile_fileno();
    if (fd != -1)
        close(fd);
    vmp_set_profile_fileno(-1);
}

static int install_pthread_atfork_hooks(void) {
    /* this is needed to prevent the problems described there:
         - http://code.google.com/p/gperftools/issues/detail?id=278
         - http://lists.debian.org/debian-glibc/2010/03/msg00161.html

        TL;DR: if the RSS of the process is large enough, the clone() syscall
        will be interrupted by the SIGPROF before it can complete, then
        retried, interrupted again and so on, in an endless loop.  The
        solution is to disable the timer around the fork, and re-enable it
        only inside the parent.
    */
    if (atfork_hook_installed)
        return 0;
    int ret = pthread_atfork(atfork_disable_timer, atfork_enable_timer, atfork_close_profile_file);
    if (ret != 0)
        return -1;
    atfork_hook_installed = 1;
    return 0;
}

RPY_EXTERN
int vmprof_enable(int memory)
{
    assert(vmp_profile_fileno() >= 0);
    assert(prepare_interval_usec > 0);
    profile_interval_usec = prepare_interval_usec;
    if (memory && setup_rss() == -1)
        goto error;
    if (install_pthread_atfork_hooks() == -1)
        goto error;
    if (install_sigprof_handler() == -1)
        goto error;
    if (install_sigprof_timer() == -1)
        goto error;
    vmprof_ignore_signals(0);
    return 0;

 error:
    vmp_set_profile_fileno(-1);
    profile_interval_usec = 0;
    return -1;
}


static int close_profile(void)
{
    (void)vmp_write_time_now(MARKER_TRAILER);

    teardown_rss();
    /* don't close() the file descriptor from here */
    vmp_set_profile_fileno(-1);
    return 0;
}

RPY_EXTERN
int vmprof_disable(void)
{
    vmprof_ignore_signals(1);
    profile_interval_usec = 0;
    // dump all known native symbols
    dump_all_known_symbols(vmp_profile_fileno());

    if (remove_sigprof_timer() == -1)
        return -1;
    if (remove_sigprof_handler() == -1)
        return -1;
    flush_codes();
    if (shutdown_concurrent_bufs(vmp_profile_fileno()) < 0)
        return -1;
    return close_profile();
}

RPY_EXTERN
int vmprof_register_virtual_function(char *code_name, long code_uid,
                                     int auto_retry)
{
    long namelen = strnlen(code_name, 1023);
    long blocklen = 1 + 2 * sizeof(long) + namelen;
    struct profbuf_s *p;
    char *t;

 retry:
    p = current_codes;
    if (p != NULL) {
        if (__sync_bool_compare_and_swap(&current_codes, p, NULL)) {
            /* grabbed 'current_codes': we will append the current block
               to it if it contains enough room */
            size_t freesize = SINGLE_BUF_SIZE - p->data_size;
            if (freesize < (size_t)blocklen) {
                /* full: flush it */
                commit_buffer(vmp_profile_fileno(), p);
                p = NULL;
            }
        }
        else {
            /* compare-and-swap failed, don't try again */
            p = NULL;
        }
    }

    if (p == NULL) {
        p = reserve_buffer(vmp_profile_fileno());
        if (p == NULL) {
            /* can't get a free block; should almost never be the
               case.  Spin loop if allowed, or return a failure code
               if not (e.g. we're in a signal handler) */
            if (auto_retry > 0) {
                auto_retry--;
                usleep(1);
                goto retry;
            }
            return -1;
        }
    }

    t = p->data + p->data_size;
    p->data_size += blocklen;
    assert(p->data_size <= SINGLE_BUF_SIZE);
    *t++ = MARKER_VIRTUAL_IP;
    memcpy(t, &code_uid, sizeof(long)); t += sizeof(long);
    memcpy(t, &namelen, sizeof(long)); t += sizeof(long);
    memcpy(t, code_name, namelen);

    /* try to reattach 'p' to 'current_codes' */
    if (!__sync_bool_compare_and_swap(&current_codes, NULL, p)) {
        /* failed, flush it */
        commit_buffer(vmp_profile_fileno(), p);
    }
    return 0;
}

static void flush_codes(void)
{
    struct profbuf_s *p = current_codes;
    if (p != NULL) {
        current_codes = NULL;
        commit_buffer(vmp_profile_fileno(), p);
    }
}
