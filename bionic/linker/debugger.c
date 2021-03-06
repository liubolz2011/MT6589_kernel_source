/*
 * Copyright (C) 2008 The Android Open Source Project
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <signal.h>
#include <sys/prctl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>

extern int tgkill(int tgid, int tid, int sig);

void notify_gdb_of_libraries();

#define DEBUGGER_SOCKET_NAME "android:debuggerd"

#define DEBUGGER_USE_NEW 1
#if DEBUGGER_USE_NEW
typedef enum {
    // dump a crash
    DEBUGGER_ACTION_CRASH,
    // dump a tombstone file
    DEBUGGER_ACTION_DUMP_TOMBSTONE,
    // dump a backtrace only back to the socket
    DEBUGGER_ACTION_DUMP_BACKTRACE,
} debugger_action_t;

/* message sent over the socket */
typedef struct {
    debugger_action_t action;
    pid_t tid;
} debugger_msg_t;
#endif // #if DEBUGGER_USE_NEW

#define  RETRY_ON_EINTR(ret,cond) \
    do { \
        ret = (cond); \
    } while (ret < 0 && errno == EINTR)

// see man(2) prctl, specifically the section about PR_GET_NAME
#define MAX_TASK_NAME_LEN (16)

static int socket_abstract_client(const char *name, int type)
{
    struct sockaddr_un addr;
    size_t namelen;
    socklen_t alen;
    int s, err;

    namelen  = strlen(name);

    // Test with length +1 for the *initial* '\0'.
    if ((namelen + 1) > sizeof(addr.sun_path)) {
        errno = EINVAL;
        return -1;
    }

    /* This is used for abstract socket namespace, we need
     * an initial '\0' at the start of the Unix socket path.
     *
     * Note: The path in this case is *not* supposed to be
     * '\0'-terminated. ("man 7 unix" for the gory details.)
     */
    memset (&addr, 0, sizeof addr);
    addr.sun_family = AF_LOCAL;
    addr.sun_path[0] = 0;
    memcpy(addr.sun_path + 1, name, namelen);

    alen = namelen + offsetof(struct sockaddr_un, sun_path) + 1;

    s = socket(AF_LOCAL, type, 0);
    if(s < 0) return -1;

    RETRY_ON_EINTR(err,connect(s, (struct sockaddr *) &addr, alen));
    if (err < 0) {
        close(s);
        s = -1;
    }

    return s;
}

#include "linker_format.h"
#include <../libc/private/logd.h>

/*
 * Writes a summary of the signal to the log file.  We do this so that, if
 * for some reason we're not able to contact debuggerd, there is still some
 * indication of the failure in the log.
 *
 * We could be here as a result of native heap corruption, or while a
 * mutex is being held, so we don't want to use any libc functions that
 * could allocate memory or hold a lock.
 *
 * "info" will be NULL if the siginfo_t information was not available.
 */
static void logSignalSummary(int signum, const siginfo_t* info)
{
    char buffer[128];
    char threadname[MAX_TASK_NAME_LEN + 1]; // one more for termination

    char* signame;
    switch (signum) {
        case SIGILL:    signame = "SIGILL";     break;
        case SIGABRT:   signame = "SIGABRT";    break;
        case SIGBUS:    signame = "SIGBUS";     break;
        case SIGFPE:    signame = "SIGFPE";     break;
        case SIGSEGV:   signame = "SIGSEGV";    break;
#if defined(SIGSTKFLT)
        case SIGSTKFLT: signame = "SIGSTKFLT";  break;
#endif
        case SIGPIPE:   signame = "SIGPIPE";    break;
        default:        signame = "???";        break;
    }

    if (prctl(PR_GET_NAME, (unsigned long)threadname, 0, 0, 0) != 0) {
        strcpy(threadname, "<name unknown>");
    } else {
        // short names are null terminated by prctl, but the manpage
        // implies that 16 byte names are not.
        threadname[MAX_TASK_NAME_LEN] = 0;
    }
    if (info != NULL) {
        format_buffer(buffer, sizeof(buffer),
            "Fatal signal %d (%s) at 0x%08x (code=%d), thread %d (%s)",
            signum, signame, info->si_addr, info->si_code, gettid(), threadname);
    } else {
        format_buffer(buffer, sizeof(buffer),
            "Fatal signal %d (%s), thread %d (%s)",
            signum, signame, gettid(), threadname);
    }

    __libc_android_log_write(ANDROID_LOG_FATAL, "libc", buffer);
}

/*
 * Returns true if the handler for signal "signum" has SA_SIGINFO set.
 */
static bool haveSiginfo(int signum)
{
    struct sigaction oldact, newact;

    memset(&newact, 0, sizeof(newact));
    newact.sa_handler = SIG_DFL;
    newact.sa_flags = SA_RESTART;
    sigemptyset(&newact.sa_mask);

    if (sigaction(signum, &newact, &oldact) < 0) {
        __libc_android_log_write(ANDROID_LOG_FATAL, "libc",
            "Failed testing for SA_SIGINFO");
        return 0;
    }
    bool ret = (oldact.sa_flags & SA_SIGINFO) != 0;

    if (sigaction(signum, &oldact, NULL) < 0) {
        __libc_android_log_write(ANDROID_LOG_FATAL, "libc",
            "Restore failed in test for SA_SIGINFO");
    }
    return ret;
}

/*
 * Catches fatal signals so we can ask debuggerd to ptrace us before
 * we crash.
 */
void debugger_signal_handler(int n, siginfo_t* info, void* unused __attribute__((unused)))
{
    char msgbuf[128];
    unsigned tid;
    int s;

    /*
     * It's possible somebody cleared the SA_SIGINFO flag, which would mean
     * our "info" arg holds an undefined value.
     */
    if (!haveSiginfo(n)) {
        info = NULL;
    }

    logSignalSummary(n, info);

    tid = gettid();
    s = socket_abstract_client(DEBUGGER_SOCKET_NAME, SOCK_STREAM);

    if (s >= 0) {
        /* debugger knows our pid from the credentials on the
         * local socket but we need to tell it our tid.  It
         * is paranoid and will verify that we are giving a tid
         * that's actually in our process
         */
        int  ret;
    #if DEBUGGER_USE_NEW
        debugger_msg_t msg;
        msg.action = DEBUGGER_ACTION_CRASH;
        msg.tid = tid;
        RETRY_ON_EINTR(ret, write(s, &msg, sizeof(msg)));
        if (ret == sizeof(msg)) {
    #else
        RETRY_ON_EINTR(ret, write(s, &tid, sizeof(unsigned)));
        if (ret == sizeof(unsigned)) {
    #endif // #if DEBUGGER_USE_NEW
            /* if the write failed, there is no point to read on
             * the file descriptor. */
            RETRY_ON_EINTR(ret, read(s, &tid, 1));
            int savedErrno = errno;
            notify_gdb_of_libraries();
            errno = savedErrno;
        }

        if (ret < 0) {
            /* read or write failed -- broken connection? */
            format_buffer(msgbuf, sizeof(msgbuf),
                "Failed while talking to debuggerd: %s", strerror(errno));
            __libc_android_log_write(ANDROID_LOG_FATAL, "libc", msgbuf);
        }

        close(s);
    } else {
        /* socket failed; maybe process ran out of fds */
        format_buffer(msgbuf, sizeof(msgbuf),
            "Unable to open connection to debuggerd: %s", strerror(errno));
        __libc_android_log_write(ANDROID_LOG_FATAL, "libc", msgbuf);
    }

    /* remove our net so we fault for real when we return */
    signal(n, SIG_DFL);

    /*
     * These signals are not re-thrown when we resume.  This means that
     * crashing due to (say) SIGPIPE doesn't work the way you'd expect it
     * to.  We work around this by throwing them manually.  We don't want
     * to do this for *all* signals because it'll screw up the address for
     * faults like SIGSEGV.
     */
    switch (n) {
        case SIGABRT:
        case SIGFPE:
        // never process SIGPIPE, reference below
        //case SIGPIPE:
#ifdef SIGSTKFLT
        case SIGSTKFLT:
#endif
            (void) tgkill(getpid(), gettid(), n);
            break;
        default:    // SIGILL, SIGBUS, SIGSEGV
            break;
    }
}

void debugger_init()
{
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_sigaction = debugger_signal_handler;
    act.sa_flags = SA_RESTART | SA_SIGINFO;
    sigemptyset(&act.sa_mask);

    sigaction(SIGILL, &act, NULL);
    sigaction(SIGABRT, &act, NULL);
    sigaction(SIGBUS, &act, NULL);
    sigaction(SIGFPE, &act, NULL);
    sigaction(SIGSEGV, &act, NULL);
#if defined(SIGSTKFLT)
    sigaction(SIGSTKFLT, &act, NULL);
#endif
    //sigaction(SIGPIPE, &act, NULL);
    /*
     * [by bo.song 2011/12/09]
     *      we will ignore SIGPIPE, bacause although this signal will be caught
     *      by debuggerd, but it will not be handled actually. And it may block
     *      debuggerd by 3 seconds in ICS, so we will ignore it here.
    */
    signal(SIGPIPE, SIG_IGN);
}
