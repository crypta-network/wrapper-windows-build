/*
 * Copyright (c) 1999, 2025 Tanuki Software, Ltd.
 * http://www.tanukisoftware.com
 * All rights reserved.
 *
 * This software is the proprietary information of Tanuki Software.
 * You shall use it only in accordance with the terms of the
 * license agreement you entered into with Tanuki Software.
 * http://wrapper.tanukisoftware.com/doc/english/licenseOverview.html
 *
 *
 * Portions of the Software have been derived from source code
 * developed by Silver Egg Technology under the following license:
 *
 * Copyright (c) 2001 Silver Egg Technology
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sub-license, and/or
 * sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 */

/**
 * Author:
 *   Leif Mortenson <leif@tanukisoftware.com>
 *   Ryan Shaw
 */

#ifndef WIN32

#ifdef LINUX
 #include <features.h>
 #include <gnu/libc-version.h>
#endif
#include <wchar.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <limits.h>
#include <pthread.h>
#include <pwd.h>
#include <sys/ioctl.h>
#ifdef SOLARIS
 #include <sys/filio.h> /* for FIONREAD */
#endif
#include <sys/resource.h>
#include <sys/timeb.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "wrapper_i18n.h"
#include "wrapper.h"
#include "wrapperinfo.h"
#include "property.h"
#include "logger.h"
#include "logger_file.h"
#include "wrapper_file.h"
#include "wrapper_jvm_launch.h"
#include "wrapper_encoding.h"

#include <sys/time.h>
#if defined(LINUX) || defined(MACOSX) || defined(AIX)
 #include <sys/select.h>
#endif

#ifndef USE_USLEEP
 #include <time.h>
#endif

#ifndef getsid
/* getpid links ok on Linux, but is not defined correctly. */
pid_t getsid(pid_t pid);
#endif

#define max(x,y) (((x) > (y)) ? (x) : (y))
#define min(x,y) (((x) < (y)) ? (x) : (y))


/* Define a global pipe descriptor so that we don't have to keep allocating
 *  a new pipe each time a JVM is launched. */
int pipedes[2] = {-1, -1};
int pipeind[2] = {-1, -1}; /* pipe descriptor for stdin */
#define PIPE_READ_END 0
#define PIPE_WRITE_END 1

/**
 * maximum length for a user name should be 8, 
 * but according to 'man useradd' it may be 32
 */
#define MAX_USER_NAME_LENGTH 32

TCHAR wrapperClasspathSeparator = TEXT(':');

pthread_t javaIOThreadId;
int javaIOThreadStarted = FALSE;
int stopJavaIOThread = FALSE;
int javaIOThreadStopped = FALSE;

pthread_t javaINThreadId;
int javaINThreadStarted = FALSE;
int stopJavaINThread = FALSE;
int javaINThreadStopped = FALSE;

int timerThreadSet = FALSE;
pthread_t timerThreadId;
int timerThreadStarted = FALSE;
int stopTimerThread = FALSE;
int timerThreadStopped = FALSE;

TICKS timerTicks = WRAPPER_TICK_INITIAL;

TICKS stopSignalLastTick = WRAPPER_TICK_INITIAL;

/** Flag which keeps track of whether or not PID files should be deleted on shutdown. */
int cleanUpPIDFilesOnExit = FALSE;

/******************************************************************************
 * Platform specific methods
 *****************************************************************************/

/**
 * exits the application after running shutdown code.
 */
void appExit(int exitCode, int argc, TCHAR** argv) {
    static int isExiting = FALSE;
    int i;
    
    /* Avoid being called more than once. */
    if (isExiting) {
        return;
    }
    isExiting = TRUE;

    /* We only want to delete the pid files if we created them. Some Wrapper
     *  invocations are meant to run in parallel with Wrapper instances
     *  controlling a JVM. */
    if (cleanUpPIDFilesOnExit) {
        /* Remove pid file.  It may no longer exist. */
        if (wrapperData->pidFilename) {
            _tunlink(wrapperData->pidFilename);
        }

        /* Remove lock file.  It may no longer exist. */
        if (wrapperData->lockFilename) {
            _tunlink(wrapperData->lockFilename);
        }

        /* Remove status file.  It may no longer exist. */
        if (wrapperData->statusFilename) {
            _tunlink(wrapperData->statusFilename);
        }

        /* Remove java status file if it was registered and created by this process. */
        if (wrapperData->javaStatusFilename) {
            _tunlink(wrapperData->javaStatusFilename);
        }

        /* Remove java id file if it was registered and created by this process. */
        if (wrapperData->javaIdFilename) {
            _tunlink(wrapperData->javaIdFilename);
        }

        /* Remove anchor file.  It may no longer exist. */
        if (wrapperData->anchorFilename) {
            _tunlink(wrapperData->anchorFilename);
        }
    }

    /* Common wrapper cleanup code. */
    wrapperDispose(exitCode);
#if defined(UNICODE)
    for (i = 0; i < argc; i++) {
        if (argv[i]) {
            free(argv[i]);
        }
    }
    if (argv) {
        free(argv);
    }
#endif
    exit(exitCode);
}

void changePidFileGroup(const TCHAR* filename, gid_t newGroup) {
    if (filename && (newGroup != -1)) {
        if (_tchown(filename, -1, newGroup) == -1) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT("Unable to change the group of %s. %s"),
                filename, getLastErrorText());
        }
    }
}

/**
 * Writes a PID to disk.
 *
 * @param filename File to write to.
 * @param pid      pid to write in the file.
 * @param newUmask Umask to use when creating the file.
 * @param newGroup Group to use when creating the file.
 *
 * @return 1 if there was an error, 0 if Ok.
 */
int writePidFile(const TCHAR *filename, DWORD pid, int newUmask, gid_t newGroup) {
    FILE *pid_fp = NULL;
    int old_umask;

    old_umask = umask(newUmask);
    pid_fp = _tfopen(filename, TEXT("w"));
    umask(old_umask);

    if (pid_fp != NULL) {
        changePidFileGroup(filename, newGroup);
        _ftprintf(pid_fp, TEXT("%d\n"), (int)pid);
        fclose(pid_fp);
    } else {
        return 1;
    }
    return 0;
}

/**
 * Send a signal to the JVM process asking it to dump its JVM state.
 */
void wrapperRequestDumpJVMState() {
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS,
        TEXT("Dumping JVM state."));
    if (wrapperData->javaPID == 0) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS,
                   TEXT("JVM is currently not running."));
    } else if (kill(wrapperData->javaPID, SIGQUIT) < 0) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR,
                   TEXT("Could not dump JVM state: %s"), getLastErrorText());
    }
}

/**
 * Called when a signal is processed.  This is actually called from within the main event loop
 *  and NOT the signal handler.  So it is safe to use the normal logging functions.
 *
 * @param sigNum Signal that was fired.
 * @param sigName Name of the signal for logging.
 * @param mode Action that should be taken.
 */
void takeSignalAction(int sigNum, const TCHAR *sigName, int mode) {
    if (wrapperData->ignoreSignals & WRAPPER_IGNORE_SIGNALS_WRAPPER) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS,
            TEXT("%s trapped, but ignored."), sigName);
    } else {
        switch (mode) {
        case WRAPPER_SIGNAL_MODE_RESTART:
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS,
                TEXT("%s trapped.  %s"), sigName, wrapperGetRestartProcessMessage());
            wrapperRestartProcess();
            break;

        case WRAPPER_SIGNAL_MODE_SHUTDOWN:
            if (wrapperData->exitRequested || wrapperData->restartRequested ||
                (wrapperData->jState == WRAPPER_JSTATE_DOWN_CLEAN) ||
                (wrapperData->jState == WRAPPER_JSTATE_STOP) ||
                (wrapperData->jState == WRAPPER_JSTATE_STOPPING) ||
                (wrapperData->jState == WRAPPER_JSTATE_STOPPED) ||
                (wrapperData->jState == WRAPPER_JSTATE_KILLING) ||
                (wrapperData->jState == WRAPPER_JSTATE_KILL) ||
                (wrapperData->jState == WRAPPER_JSTATE_DOWN_CHECK) ||
                (wrapperData->jState == WRAPPER_JSTATE_DOWN_FLUSH_STDIN) ||
                (wrapperData->jState == WRAPPER_JSTATE_DOWN_FLUSH)) {

                /* Signaled while we were already shutting down. */
                if ((stopSignalLastTick == WRAPPER_TICK_INITIAL) || (wrapperGetTickAgeTicks(stopSignalLastTick, wrapperGetTicks()) >= wrapperData->forcedShutdownDelay)) {
                    /* We want to ignore double signals which can be sent both by the script and the systems at almost the same time. */
                    if (wrapperData->isForcedShutdownDisabled) {
                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS,
                            TEXT("%s trapped.  Already shutting down."), sigName);
                    } else {
                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS,
                            TEXT("%s trapped.  Forcing immediate shutdown."), sigName);

                        /* Disable the stats and thread dump on exit feature if it is set because it
                         *  should not be displayed when the user requested the immediate exit. */
                        wrapperData->requestThreadDumpOnFailedJVMExit = FALSE;
                        wrapperKillProcess(FALSE);
                    }
                }
            } else {
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS,
                    TEXT("%s trapped.  Shutting down."), sigName);
                /* Always force the shutdown as this is an external event. */
                wrapperStopProcess(0, TRUE);
                stopSignalLastTick = wrapperGetTicks();
            }
            /* Don't actually kill the process here.  Let the application shut itself down */

            /* To make sure that the JVM will not be restarted for any reason,
             *  start the Wrapper shutdown process as well. */
            if ((wrapperData->wState == WRAPPER_WSTATE_STOPPING) ||
                (wrapperData->wState == WRAPPER_WSTATE_STOPPED)) {
                /* Already stopping. */
            } else {
                wrapperSetWrapperState(WRAPPER_WSTATE_STOPPING);
            }
            break;

        case WRAPPER_SIGNAL_MODE_FORWARD:
            if (wrapperData->javaPID > 0) {
                if (wrapperData->isDebugging) {
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG,
                        TEXT("%s (%d) trapped.  Forwarding to JVM process."), sigName, sigNum);
                }
                if (kill(wrapperData->javaPID, sigNum)) {
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN,
                        TEXT("Unable to forward %s signal to JVM process.  %s"), sigName, getLastErrorText());
                }
            } else {
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS,
                    TEXT("%s trapped.  Unable to forward signal to JVM because it is not running."), sigName);
            }
            break;

        case WRAPPER_SIGNAL_MODE_PAUSE:
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS,
                TEXT("%s trapped.  %s"), sigName, wrapperGetPauseProcessMessage());
            wrapperPauseProcess(WRAPPER_ACTION_SOURCE_CODE_SIGNAL);
            break;

        case WRAPPER_SIGNAL_MODE_RESUME:
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS,
                TEXT("%s trapped.  %s"), sigName, wrapperGetResumeProcessMessage());
            wrapperResumeProcess(WRAPPER_ACTION_SOURCE_CODE_SIGNAL);
            break;

        case WRAPPER_SIGNAL_MODE_CLOSE_LOGFILE:
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS,
                TEXT("%s trapped.  Closing the log file."), sigName);
            flushLogfile();
            closeLogfile();
            break;

        default: /* WRAPPER_SIGNAL_MODE_IGNORE */
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS,
                TEXT("%s trapped, but ignored."), sigName);
            break;
        }
    }
}

/**
 * This function goes through and checks flags for each of several signals to see if they
 *  have been fired since the last time this function was called.  This is the only thread
 *  which will ever clear these flags, but they can be set by other threads within the
 *  signal handlers at ANY time.  So only check the value of each flag once and reset them
 *  immediately to decrease the chance of missing duplicate signals.
 */
int wrapperMaintainSignals() {
    int quit = FALSE;

    if (!handleSignals) {
        return FALSE;
    }
    
    /* SIGINT */
    if (wrapperData->signalInterruptTrapped) {
        wrapperData->signalInterruptTrapped = FALSE;
        
        takeSignalAction(SIGINT, TEXT("INT"), WRAPPER_SIGNAL_MODE_SHUTDOWN);
        quit = TRUE;
    }
    
    /* SIGQUIT */
    if (wrapperData->signalQuitTrapped) {
        wrapperData->signalQuitTrapped = FALSE;
        
        if (wrapperData->signalQuitSkip) {
            /* When CTRL+'\' is captured by the terminal driver (in the kernel), SIGQUIT
             *  is sent to the foreground process group of the current session.
             *  Since the JVM and Wrapper processes belong to the same process group,
             *  the JVM would receive the signal twice if we forward it. Instead, just log
             *  a message and let the JVM handle the signal on its own. */
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("Dumping JVM state."));
            wrapperData->signalQuitSkip = FALSE;
        } else {
            wrapperRequestDumpJVMState();
        }
    }
    
    /* SIGCHLD */
    if (wrapperData->signalChildTrapped) {
        wrapperData->signalChildTrapped = FALSE;
        
        if (wrapperData->signalChildContinuedTrapped) {
            wrapperCheckAndUpdateProcessStatus(wrapperGetTicks(), TRUE);
            wrapperData->signalChildContinuedTrapped = FALSE;
        } else {
            wrapperCheckAndUpdateProcessStatus(wrapperGetTicks(), FALSE);
        }
    }
    
    /* SIGTERM */
    if (wrapperData->signalTermTrapped) {
        wrapperData->signalTermTrapped = FALSE;
        
        takeSignalAction(SIGTERM, TEXT("TERM"), WRAPPER_SIGNAL_MODE_SHUTDOWN);
        quit = TRUE;
    }
    
    /* SIGHUP */
    if (wrapperData->signalHUPTrapped) {
        wrapperData->signalHUPTrapped = FALSE;
        
        takeSignalAction(SIGHUP, TEXT("HUP"), wrapperData->signalHUPMode);
        if (wrapperData->signalHUPMode == WRAPPER_SIGNAL_MODE_SHUTDOWN) {
            quit = TRUE;
        }
    }
    
    /* SIGUSR1 */
    if (wrapperData->signalUSR1Trapped) {
        wrapperData->signalUSR1Trapped = FALSE;
        
        takeSignalAction(SIGUSR1, TEXT("USR1"), wrapperData->signalUSR1Mode);
        if (wrapperData->signalUSR1Mode == WRAPPER_SIGNAL_MODE_SHUTDOWN) {
            quit = TRUE;
        }
    }
    
#ifndef VALGRIND
    /* SIGUSR2 */
    if (wrapperData->signalUSR2Trapped) {
        wrapperData->signalUSR2Trapped = FALSE;
        
        takeSignalAction(SIGUSR2, TEXT("USR2"), wrapperData->signalUSR2Mode);
        if (wrapperData->signalUSR2Mode == WRAPPER_SIGNAL_MODE_SHUTDOWN) {
            quit = TRUE;
        }
    }
#endif
    return quit;
}

/**
 * This is called from within signal handlers so NO MALLOCs are allowed here.
 */
const TCHAR* getSignalName(int signo) {
    switch (signo) {
    case SIGALRM:
        return TEXT("SIGALRM");
    case SIGINT:
        return TEXT("SIGINT");
    case SIGKILL:
        return TEXT("SIGKILL");
    case SIGQUIT:
        return TEXT("SIGQUIT");
    case SIGCHLD:
        return TEXT("SIGCHLD");
    case SIGTERM:
        return TEXT("SIGTERM");
    case SIGHUP:
        return TEXT("SIGHUP");
    case SIGUSR1:
        return TEXT("SIGUSR1");
    case SIGUSR2:
        return TEXT("SIGUSR2");
    case SIGSEGV:
        return TEXT("SIGSEGV");
    default:
        return TEXT("UNKNOWN");
    }
}

/**
 * This is called from within signal handlers so NO MALLOCs are allowed here.
 */
const TCHAR* getSignalCodeDesc(int code) {
    switch (code) {
#ifdef SI_USER
    case SI_USER:
        return TEXT("kill, sigsend or raise");
#endif

#ifdef SI_KERNEL
    case SI_KERNEL:
        return TEXT("the kernel");
#endif

    case SI_QUEUE:
        return TEXT("sigqueue");

#ifdef SI_TIMER
    case SI_TIMER:
        return TEXT("timer expired");
#endif

#ifdef SI_MESGQ
    case SI_MESGQ:
        return TEXT("mesq state changed");
#endif

    case SI_ASYNCIO:
        return TEXT("AIO completed");

#ifdef SI_SIGIO
    case SI_SIGIO:
        return TEXT("queued SIGIO");
#endif

    default:
        return TEXT("unknown");
    }
}

/**
 * Describe a signal.  This is called from within signal handlers so NO MALLOCs are allowed here.
 */
void descSignal(siginfo_t *sigInfo) {
#ifdef SI_USER
    struct passwd *pw;
 #ifdef UNICODE
    size_t req;
 #endif
    TCHAR uName[MAX_USER_NAME_LENGTH + 1];
#endif

    /* Not supported on all platforms */
    if (sigInfo == NULL) {
        log_printf_queue(TRUE, WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG,
            TEXT("Signal trapped.  No details available."));
        return;
    }

    if (wrapperData->isDebugging) {
        log_printf_queue(TRUE, WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG,
            TEXT("Signal trapped.  Details:"));

        log_printf_queue(TRUE, WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG,
#if defined(UNICODE)
            TEXT("  signal number=%d (%S), source=\"%S\""),
#else
            TEXT("  signal number=%d (%s), source=\"%s\""),
#endif
            sigInfo->si_signo,
            getSignalName(sigInfo->si_signo),
            getSignalCodeDesc(sigInfo->si_code));

        if (sigInfo->si_errno != 0) {
            log_printf_queue(TRUE, WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG,
#if defined(UNICODE)
                TEXT("  signal err=%d, \"%S\""),
#else
                TEXT("  signal err=%d, \"%s\""),
#endif
                sigInfo->si_errno,
                strerror(sigInfo->si_errno));
        }
            
#ifdef SI_USER
        if (sigInfo->si_code == SI_USER) {
            pw = getpwuid(sigInfo->si_uid);
            if (pw == NULL) {
                _sntprintf(uName, MAX_USER_NAME_LENGTH + 1, TEXT("<unknown>"));
            } else {
 #ifndef UNICODE
                _sntprintf(uName, MAX_USER_NAME_LENGTH + 1, TEXT("%s"), pw->pw_name);
 #else
                req = mbstowcs(NULL, pw->pw_name, MBSTOWCS_QUERY_LENGTH);
                if (req == (size_t)-1) {
                    return;
                }
                if (req > MAX_USER_NAME_LENGTH) {
                    req = MAX_USER_NAME_LENGTH;
                }
                mbstowcs(uName, pw->pw_name, req + 1);
                uName[req] = TEXT('\0'); /* Avoid bufferflows caused by badly encoded characters. */
 #endif
            }

            /* It appears that the getsid function was added in version 1.3.44 of the linux kernel. */
            log_printf_queue(TRUE, WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG,
 #ifdef UNICODE
                TEXT("  signal generated by PID: %d (Session PID: %d), UID: %d (%S)"),
 #else
                TEXT("  signal generated by PID: %d (Session PID: %d), UID: %d (%s)"),
 #endif
                sigInfo->si_pid, getsid(sigInfo->si_pid), sigInfo->si_uid, uName);
        }
#endif
    }
}

/**
 * This function is only used for debugging and should always return FALSE
 *  if the signal was correctly masked for the threads used by the Wrapper.
 */
static int checkSignalThreads(pthread_t currentThreadId, const TCHAR* signalName, const TCHAR* message) {
    /* The messages here intentionally do not extract the thread name and use a common resource because it makes them difficult to localize cleanly. */
    if (timerThreadSet && pthread_equal(currentThreadId, timerThreadId)) {
        log_printf_queue(TRUE, WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("The Timer thread received a '%s' signal.%s"), signalName, message);
    } else if (javaINThreadStarted && pthread_equal(currentThreadId, javaINThreadId)) {
        log_printf_queue(TRUE, WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("The JavaIN thread received a '%s' signal.%s"), signalName, message);
    } else if (javaIOThreadStarted && pthread_equal(currentThreadId, javaIOThreadId)) {
        log_printf_queue(TRUE, WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("The JavaIO thread received a '%s' signal.%s"), signalName, message);
    } else {
        return FALSE;
    }
    return TRUE;
}

/**
 * Handle alarm signals.  We are getting them on solaris when running with
 *  the tick timer.  Not yet sure where they are coming from.
 */
void sigActionAlarm(int sigNum, siginfo_t *sigInfo, void *na) {
    /* On UNIX the calling thread is the actual thread being interrupted
     *  so it has already been registered with logRegisterThread. */

    descSignal(sigInfo);

    if (wrapperData->isDebugging) {
        if (!checkSignalThreads(pthread_self(), TEXT("SIGALRM"), TEXT("  Ignoring."))) {
            log_printf_queue(TRUE, WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG,
                TEXT("Received a '%s' signal.%s"), TEXT("SIGALRM"), TEXT("  Ignoring."));
        }
    }
}

/**
 * Handle interrupt signals (i.e. Crtl-C).
 */
void sigActionInterrupt(int sigNum, siginfo_t *sigInfo, void *na) {
    /* On UNIX the calling thread is the actual thread being interrupted
     *  so it has already been registered with logRegisterThread. */

    descSignal(sigInfo);

    wrapperData->signalInterruptTrapped = TRUE;
}

/**
 * Handle quit signals (i.e. Crtl-\).
 */
void sigActionQuit(int sigNum, siginfo_t *sigInfo, void *na) {
    /* On UNIX the calling thread is the actual thread being interrupted
     *  so it has already been registered with logRegisterThread. */

    descSignal(sigInfo);

    if (wrapperData->isDebugging) {
        checkSignalThreads(pthread_self(), TEXT("SIGQUIT"), TEXT(""));
    }
    
    wrapperData->signalQuitTrapped = TRUE;
#ifdef SI_KERNEL
    if (!wrapperData->javaNewProcessGroup && (sigInfo->si_code == SI_KERNEL)) {
        /* On Linux, when CTRL-\ is pressed, the signal is caught & dispatched by the kernel to the process group members.
         *  If the Wrapper and JVM are running in the same process group, skip forwarding the signal to avoid double handling.
         *  This would however only work on Linux and not when a signal is sent via killpg or kill with a negative pid.
         *  A better solution is to run the JVM in a separate process group.
         *  NOTE: This flag is always reset to FALSE when signals are maintained. */
        wrapperData->signalQuitSkip = TRUE;
    }
#endif
}

/**
 * Handle termination signals (i.e. machine is shutting down).
 */
void sigActionChildDeath(int sigNum, siginfo_t *sigInfo, void *na) {
    /* On UNIX, when a Child process changes state, a SIGCHLD signal is sent to the parent.
     *  The parent should do a wait to make sure the child is cleaned up and doesn't become
     *  a zombie process. */

    descSignal(sigInfo);

    if (wrapperData->isDebugging) {
        if (!checkSignalThreads(pthread_self(), TEXT("SIGCHLD"), TEXT("  Checking JVM process status."))) {
            log_printf_queue(TRUE, WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG,
                TEXT("Received a '%s' signal.%s"), TEXT("SIGCHLD"), TEXT("  Checking JVM process status."));
        }
    }
    
    /* This is set whenever any child signals that it has exited.
     *  Inside the code we go on to check to make sure that we only test for the JVM.
     *  Note: Can we test 'sigInfo->si_pid == wrapperData->javaPID'?
     *        Is it supported on all platforms? No risk of returning 0 or a default value?
     *        If we could distinguish between JVM from Event commands, it would also allow
     *        us to figure out more efficiently when the laters complete, instead of polling. */
    wrapperData->signalChildTrapped = TRUE;
    if (sigInfo->si_code == CLD_CONTINUED) {
        wrapperData->signalChildContinuedTrapped = TRUE;
    }
}

/**
 * Handle termination signals (i.e. machine is shutting down).
 */
void sigActionTermination(int sigNum, siginfo_t *sigInfo, void *na) {
    /* On UNIX the calling thread is the actual thread being interrupted
     *  so it has already been registered with logRegisterThread. */

    descSignal(sigInfo);
    
    wrapperData->signalTermTrapped = TRUE;
}

/**
 * Handle hangup signals.
 */
void sigActionHangup(int sigNum, siginfo_t *sigInfo, void *na) {
    /* On UNIX the calling thread is the actual thread being interrupted
     *  so it has already been registered with logRegisterThread. */

    descSignal(sigInfo);
    
    wrapperData->signalHUPTrapped = TRUE;
}

/**
 * Handle USR1 signals.
 */
void sigActionUSR1(int sigNum, siginfo_t *sigInfo, void *na) {
    /* On UNIX the calling thread is the actual thread being interrupted
     *  so it has already been registered with logRegisterThread. */

    descSignal(sigInfo);
    
    wrapperData->signalUSR1Trapped = TRUE;
}

/**
 * Handle USR2 signals.
 */
void sigActionUSR2(int sigNum, siginfo_t *sigInfo, void *na) {
    /* On UNIX the calling thread is the actual thread being interrupted
     *  so it has already been registered with logRegisterThread. */

    descSignal(sigInfo);
    
    wrapperData->signalUSR2Trapped = TRUE;
}

/**
 * Registers a single signal handler.
 */
int registerSigAction(int sigNum, void (*sigAction)(int, siginfo_t *, void *)) {
    struct sigaction newAct;

    newAct.sa_sigaction = sigAction;
    sigemptyset(&newAct.sa_mask);
    newAct.sa_flags = SA_SIGINFO;

    if (sigaction(sigNum, &newAct, NULL)) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL,
            TEXT("Unable to register signal handler for signal %d.  %s"), sigNum, getLastErrorText());
        return 1;
    }
    return 0;
}

/**
 * Close the pipe used to redirect stdin to the JVM;
 */
void closeStdinPipe() {
    if (pipeind[PIPE_READ_END] != -1) {
        close(pipeind[PIPE_READ_END]);
        pipeind[PIPE_READ_END] = -1;
    }
    if (pipeind[PIPE_WRITE_END] != -1) {
        close(pipeind[PIPE_WRITE_END]);
        pipeind[PIPE_WRITE_END] = -1;
    }
}

/**
 * The main entry point for the javaIN thread which is started by
 *  initializeJavaIN().  Once started, this thread will run for the
 *  life of the process.
 *  This thread is used to forward stdin to the Java process when it
 *  it is launched in a new process group.  The Wrapper should remain
 *  the foreground process group to handle signals correctly, but the
 *  file descriptor associated to stdin (0) is only valid within that
 *  group.
 */
void *javaINRunner(void *arg) {
    sigset_t signal_mask;
    char *chBuf = NULL;
    char *pChBuf;
    int retS, retR, retW;
    int i;
    fd_set rfds;
    struct timeval tv;
    int skipRead = FALSE;
    
    javaINThreadStarted = TRUE;

    /* Immediately register this thread with the logger. */
    logRegisterThread(WRAPPER_THREAD_JAVAIN);

    /* mask signals so the javaIN doesn't get any of these. */
    sigemptyset(&signal_mask);
    sigaddset(&signal_mask, SIGTERM);
    sigaddset(&signal_mask, SIGINT);
    sigaddset(&signal_mask, SIGQUIT);
    sigaddset(&signal_mask, SIGALRM);
    sigaddset(&signal_mask, SIGCHLD);
    sigaddset(&signal_mask, SIGHUP);
    sigaddset(&signal_mask, SIGUSR1);
#ifndef VALGRIND
    sigaddset(&signal_mask, SIGUSR2);
#endif
    if (pthread_sigmask(SIG_BLOCK, &signal_mask, NULL) != 0) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT("Could not mask signals for %s thread."), TEXT("JavaIN"));
    }

    if (wrapperData->isJavaIOOutputEnabled) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("%s thread started."), TEXT("JavaIN"));
    }

    /* Initialize a set of file descriptors which will only contain STDIN_FILENO. */
    FD_ZERO(&rfds);
    
    /* The buffer size is fixed (can't be reloaded), so we only need to allocate once. */
    chBuf = malloc(sizeof(char) * wrapperData->javaINBufferSize);
    if (!chBuf) {
        outOfMemory(TEXT("JINR"), 1);
    } else { 
        while (!stopJavaINThread) {
            if (pipeind[PIPE_WRITE_END] == -1) {
                if ((wrapperData->jState == WRAPPER_JSTATE_LAUNCHING) ||
                    (wrapperData->jState == WRAPPER_JSTATE_LAUNCHED) ||
                    (wrapperData->jState == WRAPPER_JSTATE_STARTING) ||
                    (wrapperData->jState == WRAPPER_JSTATE_STARTED) ||
                    (wrapperData->jState == WRAPPER_JSTATE_STOP) ||
                    (wrapperData->jState == WRAPPER_JSTATE_STOPPING)) {
                    /* Java is up so normally the pipe for stdin should be open. */
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT(" Write end of Java's stdin was closed."));
                    stopJavaINThread = TRUE;
                } else {
                    wrapperSleep(WRAPPER_TICK_MS);
                    continue;
                }
            } else if (fcntl(pipeind[PIPE_WRITE_END], F_SETFL, O_SYNC) == -1) { /* Make sure blocking mode is set on pipeind[PIPE_WRITE_END].  Write() will be interrupted if Java goes down. */
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT("Failed to set write mode for Java's stdin.  %s"), getLastErrorText());
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT(" Disable stdin."));
                
                /* NOTE: It would be possible put this thread into sleep until a next JVM is restarted and try again fcntl on the new pipe created...
                 *       but this would consume CPU or would require a more complex logic to wake up the thread. Don't think this is needed for now. */
                stopJavaINThread = TRUE;
            } else {
                /* Java (re-)started, and the pipe conntected to its stdin was (re-)created. */

#ifdef DEBUG_JAVAIN
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("DEBUG_JAVAIN: start read/write"));
#endif
                /* Read/Write (again). Reset the flush flag. */
                wrapperData->javaINFlushed = FALSE;
                do {
                    if (skipRead) {
                        /* We already read (happens when we don't flush on a jvm restart). */
                        retS = 1;
                    } else {
                        /* Make sure STDIN_FILENO is always present in rfds. It may have been removed by select on a previous call. */
                        if (!FD_ISSET(STDIN_FILENO, &rfds)) {
                            FD_SET(STDIN_FILENO, &rfds);
                        }
                        
                        /* Set the timeout to 1 tick and reinitialize it on each invocation as it may have been updated by a previous call.
                         *  Note: An alternative would be to use pselect() which doesn't update the timeout. */
                        tv.tv_sec = 0;
                        tv.tv_usec = WRAPPER_TICK_MS * 1000;
                        
                        /* Mark this thread as ready to read. */
                        wrapperData->javaINReady = TRUE;

                        /** WAIT until there is something to read. **/
                        retS = select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv);

                        if ((retS == -1) && (errno != EINTR)) {
                            /* Error other than signal interruption. */
                            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT("Failed to wait for stdin to be ready for I/O. (0x%x)"), errno);
                            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT(" Disable stdin."));
                            stopJavaINThread = TRUE;
                            break;
                        }
                    }

                    if (retS > 0) {
                        if (skipRead) {
                            skipRead = FALSE;
                            retR = strlen(chBuf) * sizeof(char);
                        } else {
                            /* stdin is ready to be read. */
                            
                            /** READ stdin. */
#ifdef DEBUG_JAVAIN
                            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("DEBUG_JAVAIN: read(STDIN_FILENO, chBuf, %d)"), wrapperData->javaINBufferSize - 1);
#endif
                            /* EINTR is only worth testing if read() would block. Normally select() should prevent from
                             *  blocking, but in some circumstances this may not always be the case (see above). */
                            do {
                                retR = read(STDIN_FILENO, chBuf, wrapperData->javaINBufferSize - 1);
                            } while (!stopJavaINThread && (retR == -1) && (errno == EINTR));

                            if (stopJavaINThread) {
                                /* Already requested to stop. */
                                break;
                            } else if (retR == -1) {
                                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT("Failed to read from stdin.  %s"), getLastErrorText());
                                stopJavaINThread = TRUE;
                                break;
                            } else if (retR == 0) {
                                /* End of stream.  May happen for example when piping a file to the wrapper. */
                                if (wrapperData->isJavaIOOutputEnabled) {
                                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("Ended reading stdin (%d)."), 1);
                                } else {
                                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Ended reading stdin (%d)."), 1);
                                }
                                stopJavaINThread = TRUE;
                                break;
                            }
                            chBuf[retR / sizeof(char)] = 0;
                        }

                        /** WRITE to pipeind if it's still open. **/
                        if (pipeind[PIPE_WRITE_END] != -1) {
                            pChBuf = chBuf;
                            while ((retR > 0) && !stopJavaINThread) {
                                /* Write to the write-end of the pipe which is connected to Java's stdin.  We are blocking but write() will return -1 if Java goes down. */
#ifdef DEBUG_JAVAIN
                                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("DEBUG_JAVAIN: write(pipeind[PIPE_WRITE_END], pChBuf, %d)"), retR);
#endif
                                retW = write(pipeind[PIPE_WRITE_END], pChBuf, retR);
                                if ((retW == -1) && (errno != EINTR)) {
                                    /* May happen when the was JVM stopped while we were writing.  Make sure to close the pipe. */
#ifdef DEBUG_JAVAIN
                                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("DEBUG_JAVAIN: write failed (%d)"), errno);
#endif
                                    close(pipeind[PIPE_WRITE_END]);
                                    pipeind[PIPE_WRITE_END] = -1;
                                    break;
                                } else if (retW > 0) {
                                    pChBuf += (retW / sizeof(char));
                                    retR -= retW;
                                }
                            }
                        }
                    } else {
                        /* select() timed out or was interrupted by a signal.  Continue. */
                    }
                } while (!stopJavaINThread && (pipeind[PIPE_WRITE_END] != -1));
                
                /** FLUSH? **/
                if (!stopJavaINThread) {
                    if (!wrapperData->javaINFlush) {
                        skipRead = TRUE;
                    } else {
                        /* We want to empty the pipe to have a clean start of stdin for the next JVM. */
                        wrapperData->javaINFlushing = TRUE;
#ifdef DEBUG_JAVAIN
                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("DEBUG_JAVAIN: flushing started"));
#endif

                        /* Make sure non-blocking mode is set on STDIN_FILENO. This is needed because:
                         *  - if we call again read() and there is no more byte, the blocking would prevent us from setting the wrapperData->javaINFlushed flag (unless we implement a timeout?)
                         *  - if we read less than the buffer size, it's no guarantee that we reached a 'blank'. It might just mean we are reading faster than the pipe is written.
                         *  - even if we would implement a timeout, there is always the possibility that we get no bytes for a shorter time and then a new series of bytes. */
                        if (fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK) == -1) {
                            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT("Failed to set read mode for stdin.  %s"), getLastErrorText());
                            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT(" Disable stdin."));
                            stopJavaINThread = TRUE;
                        } else {
                            i = 0;
                            while (!stopJavaINThread) {
                                retR = read(STDIN_FILENO, chBuf, wrapperData->javaINBufferSize - 1);
#ifdef DEBUG_JAVAIN
                                /* To debug the flushing, it can be useful to print the number of bytes read on each loop (or error codes). */
                                _tprintf(TEXT("%d "), retR);
#endif
                                if (retR == 0) {
                                    /* End of stream.  May happen for example when piping a file to the wrapper. */
                                    if (wrapperData->isJavaIOOutputEnabled) {
                                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("Ended reading stdin (%d)."), 2);
                                    } else {
                                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Ended reading stdin (%d)."), 2);
                                    }
                                    stopJavaINThread = TRUE;
                                    break;
                                } else if (retR == -1) {
                                    /* On a non-blocking call, EAGAIN means read() would have blocked without O_NONBLOCK, and EINTR can't happen.
                                     *  When the buffer is emptied, there may be a slight delay until the pipe is refilled by the system.
                                     *  We consider the flushing is finished after 10 consecutive 'EAGAIN' every ms. */
                                    if ((errno == EAGAIN) && (i++ < 10)) {
                                        wrapperSleep(1);
                                    } else {
                                        break;
                                    }
                                } else { /* retR > 0 */
                                    /* reading (again) */
                                    if (i > 0) {
                                        i = 0;
                                    }
                                }
                            }
                            if (fcntl(STDIN_FILENO, F_SETFL, O_SYNC) == -1) {
                                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT("Failed to set read mode for stdin.  %s"), getLastErrorText());
                                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT(" Disable stdin."));
                                stopJavaINThread = TRUE;
                            }
                        }
#ifdef DEBUG_JAVAIN
                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("DEBUG_JAVAIN: flushing ended"));
#endif
                        
                        /* We have read everything. If new bytes are coming from now on, they will be for the next JVM. */
                        wrapperData->javaINFlushed = TRUE;
                        
                        /* Important: must be set after javaINFlushed, because if the two flags are FALSE there is a chance we disable stdin. See jStateDownFlushStdin(). */
                        wrapperData->javaINFlushing = FALSE;
                    }
                }
            }
        }
        free(chBuf);
    }

    closeStdinPipe();
    javaINThreadStopped = TRUE;
    wrapperData->disableConsoleInputPermanent = TRUE;
    wrapperData->disableConsoleInput = TRUE;
    if (wrapperData->isJavaIOOutputEnabled) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("%s thread stopped."), TEXT("JavaIN"));
    }
    return NULL;
}

/**
 * Creates a thread whose job is to forward stdin to the JVM when it is run in a separate process group.
 *
 * @return 1 if there were any problems, 0 otherwise.
 */
int initializeJavaIN() {
    int res;

    if (wrapperData->isJavaIOOutputEnabled) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("Launching %s thread."), TEXT("JavaIN"));
    }

    res = pthread_create(&javaINThreadId,
        NULL, /* No attributes. */
        javaINRunner,
        NULL); /* No parameters need to be passed to the thread. */
    if (res) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR,
            TEXT("Unable to create a %s thread: %d, %s"), TEXT("JavaIN"), res, getLastErrorText());
        return 1;
    } else {
        if (pthread_detach(javaINThreadId)) {
            return 1;
        }
        return 0;
    }
}

void disposeJavaIN() {
    stopJavaINThread = TRUE;
    /* Wait until the javaIN thread is actually stopped to avoid timing problems. */
    if (javaINThreadStarted) {
        while (!javaINThreadStopped) {
#ifdef _DEBUG
            wprintf(TEXT("Waiting for %s thread to stop.\n"), TEXT("JavaIN"));
#endif
            wrapperSleep(100);
        }
        pthread_cancel(javaINThreadId);
    }
}

/**
 * The main entry point for the javaio thread which is started by
 *  initializeJavaIO().  Once started, this thread will run for the
 *  life of the process.
 *
 * This thread will only be started if we are configured to use a
 *  dedicated thread to read JVM output.
 */
void *javaIORunner(void *arg) {
    sigset_t signal_mask;
    int nextSleep;

    javaIOThreadStarted = TRUE;
    
    /* Immediately register this thread with the logger. */
    logRegisterThread(WRAPPER_THREAD_JAVAIO);

    /* mask signals so the javaIO doesn't get any of these. */
    sigemptyset(&signal_mask);
    sigaddset(&signal_mask, SIGTERM);
    sigaddset(&signal_mask, SIGINT);
    sigaddset(&signal_mask, SIGQUIT);
    sigaddset(&signal_mask, SIGALRM);
    sigaddset(&signal_mask, SIGCHLD);
    sigaddset(&signal_mask, SIGHUP);
    sigaddset(&signal_mask, SIGUSR1);
#ifndef VALGRIND
    sigaddset(&signal_mask, SIGUSR2);
#endif
    if (pthread_sigmask(SIG_BLOCK, &signal_mask, NULL) != 0) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT("Could not mask signals for %s thread."), TEXT("JavaIN"));
    }

    if (wrapperData->isJavaIOOutputEnabled) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("%s thread started."), TEXT("JavaIO"));
    }

    nextSleep = TRUE;
    /* Loop until we are shutting down, but continue as long as there is more output from the JVM. */
    while ((!stopJavaIOThread) || (!nextSleep)) {
        if (nextSleep) {
            /* Sleep as little as possible. */
            wrapperSleep(1);
        }
        nextSleep = TRUE;
        
        if (wrapperData->pauseThreadJavaIO) {
            wrapperPauseThread(wrapperData->pauseThreadJavaIO, TEXT("javaio"));
            wrapperData->pauseThreadJavaIO = 0;
        }
        
        if (wrapperReadChildOutput(0)) {
            if (wrapperData->isDebugging) {
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG,
                    TEXT("Pause reading child process output to share cycles."));
            }
            nextSleep = FALSE;
        }
    }

    javaIOThreadStopped = TRUE;
    if (wrapperData->isJavaIOOutputEnabled) {
        log_printf_queue(TRUE, WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("%s thread stopped."), TEXT("JavaIO"));
    }
    return NULL;
}

/**
 * Creates a process whose job is to loop and simply increment a ticks
 *  counter.  The tick counter can then be used as a clock as an alternative
 *  to using the system clock.
 */
int initializeJavaIO() {
    int res;

    if (wrapperData->isJavaIOOutputEnabled) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("Launching %s thread."), TEXT("JavaIO"));
    }

    res = pthread_create(&javaIOThreadId,
        NULL, /* No attributes. */
        javaIORunner,
        NULL); /* No parameters need to be passed to the thread. */
    if (res) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL,
            TEXT("Unable to create a %s thread: %d, %s"), TEXT("JavaIO"), res, getLastErrorText());
        return 1;
    } else {
        if (pthread_detach(javaIOThreadId)) {
            return 1;
        }
        return 0;
    }
}

void disposeJavaIO() {
    stopJavaIOThread = TRUE;
    /* Wait until the javaIO thread is actually stopped to avoid timing problems. */
    if (javaIOThreadStarted) {
        while (!javaIOThreadStopped) {
#ifdef _DEBUG
            wprintf(TEXT("Waiting for %s thread to stop.\n"), TEXT("JavaIO"));
#endif
            wrapperSleep(100);
        }
        pthread_cancel(javaIOThreadId);
    }
}

/**
 * The main entry point for the timer thread which is started by
 *  initializeTimer().  Once started, this thread will run for the
 *  life of the process.
 *
 * This thread will only be started if we are configured NOT to
 *  use the system time as a base for the tick counter.
 */
void *timerRunner(void *arg) {
    TICKS sysTicks;
    TICKS lastTickOffset = 0;
    TICKS tickOffset;
    TICKS nowTicks;
    int offsetDiff;
    int first = TRUE;
    sigset_t signal_mask;

    timerThreadStarted = TRUE;
    
    /* Immediately register this thread with the logger. */
    logRegisterThread(WRAPPER_THREAD_TIMER);

    /* mask signals so the timer doesn't get any of these. */
    sigemptyset(&signal_mask);
    sigaddset(&signal_mask, SIGTERM);
    sigaddset(&signal_mask, SIGINT);
    sigaddset(&signal_mask, SIGQUIT);
    sigaddset(&signal_mask, SIGALRM);
    sigaddset(&signal_mask, SIGCHLD);
    sigaddset(&signal_mask, SIGHUP);
    sigaddset(&signal_mask, SIGUSR1);
#ifndef VALGRIND
    sigaddset(&signal_mask, SIGUSR2);
#endif
    if (pthread_sigmask(SIG_BLOCK, &signal_mask, NULL) != 0) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT("Could not mask signals for timer thread."));
    }

    if (wrapperData->isTickOutputEnabled) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("Timer thread started."));
    }

    wrapperGetSystemTicks();

    while (!stopTimerThread) {
        wrapperSleep(WRAPPER_TICK_MS);
        
        if (wrapperData->pauseThreadTimer) {
            wrapperPauseThread(wrapperData->pauseThreadTimer, TEXT("timer"));
            wrapperData->pauseThreadTimer = 0;
        }

        /* Get the tick count based on the system time. */
        sysTicks = wrapperGetSystemTicks();

        /* Lock the tick mutex whenever the "timerTicks" variable is accessed. */
        if (wrapperData->useTickMutex && wrapperLockTickMutex()) {
            timerThreadStopped = TRUE;
            return NULL;
        }
        
        /* Advance the timer tick count. */
        nowTicks = timerTicks++;
        
        if (wrapperData->useTickMutex && wrapperReleaseTickMutex()) {
            timerThreadStopped = TRUE;
            return NULL;
        }

        /* Calculate the offset between the two tick counts. This will always work due to overflow. */
        tickOffset = sysTicks - nowTicks;

        /* The number we really want is the difference between this tickOffset and the previous one. */
        offsetDiff = wrapperGetTickAgeTicks(lastTickOffset, tickOffset);

        if (first) {
            first = FALSE;
        } else {
            if (offsetDiff > wrapperData->timerSlowThreshold) {
                log_printf_queue(TRUE, WRAPPER_SOURCE_WRAPPER, LEVEL_INFO,
                    TEXT("The timer fell behind the system clock by %ldms."), offsetDiff * WRAPPER_TICK_MS);
            } else if (offsetDiff < -1 * wrapperData->timerFastThreshold) {
                log_printf_queue(TRUE, WRAPPER_SOURCE_WRAPPER, LEVEL_INFO,
                    TEXT("The system clock fell behind the timer by %ldms."), -1 * offsetDiff * WRAPPER_TICK_MS);
            }

            if (wrapperData->isTickOutputEnabled) {
                log_printf_queue(TRUE, WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT(
                    "    Timer: ticks=0x%08x, system ticks=0x%08x, offset=0x%08x, offsetDiff=0x%08x"),
                    nowTicks, sysTicks, tickOffset, offsetDiff);
            }
        }

        /* Store this tick offset for the next time through the loop. */
        lastTickOffset = tickOffset;
    }

    timerThreadStopped = TRUE;
    if (wrapperData->isTickOutputEnabled) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("Timer thread stopped."));
    }
    return NULL;
}

/**
 * Creates a process whose job is to loop and simply increment a ticks
 *  counter.  The tick counter can then be used as a clock as an alternative
 *  to using the system clock.
 */
int initializeTimer() {
    int res;

    if (wrapperData->isTickOutputEnabled) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("Launching Timer thread."));
    }

    res = pthread_create(&timerThreadId,
        NULL, /* No attributes. */
        timerRunner,
        NULL); /* No parameters need to be passed to the thread. */
    if (res) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL,
            TEXT("Unable to create a timer thread: %d, %s"), res, getLastErrorText());
        timerThreadSet = TRUE;
        return 1;
    } else {
        if (pthread_detach(timerThreadId)) {
            timerThreadSet = TRUE;
            return 1;
        }
        timerThreadSet = FALSE;
        return 0;
    }
}

void disposeTimer() {
    stopTimerThread = TRUE;
    /* Wait until the timer thread is actually stopped to avoid timing problems. */
    if (timerThreadStarted) {
        while (!timerThreadStopped) {
#ifdef _DEBUG
            wprintf(TEXT("Waiting for timer thread to stop.\n"));
#endif
            wrapperSleep(100);
        }
        pthread_cancel(timerThreadId);
    }
}

/**
 * Execute initialization code to get the wrapper set up.
 */
int wrapperInitializeRun() {
    int retval = 0;
    int res;
    
    /* Register any signal actions we are concerned with. */
    if (registerSigAction(SIGALRM, sigActionAlarm) ||
        registerSigAction(SIGINT,  sigActionInterrupt) ||
        registerSigAction(SIGQUIT, sigActionQuit) ||
        registerSigAction(SIGCHLD, sigActionChildDeath) ||
        registerSigAction(SIGTERM, sigActionTermination) ||
        registerSigAction(SIGHUP,  sigActionHangup) ||
        registerSigAction(SIGUSR1, sigActionUSR1)
#ifndef VALGRIND
        ||
        registerSigAction(SIGUSR2, sigActionUSR2)
#endif
        ) {
        retval = -1;
    }

    wrapperSetConsoleTitle();

    if (wrapperData->useSystemTime) {
        /* We are going to be using system time so there is no reason to start up a timer thread. */
        timerThreadSet = FALSE;
        /* Unable to set the timerThreadId to a null value on all platforms
         * timerThreadId = 0;*/
    } else {
        /* Create and initialize a timer thread. */
        if ((res = initializeTimer()) != 0) {
            return res;
        }
    }

    return retval;
}

/**
 * Cause the current thread to sleep for the specified number of milliseconds.
 *
 * @param ms Number of milliseconds to wait for.
 * @param interrupt TRUE to return when nanosleep was interrupted by a signal,
 *                  FALSE to continue sleeping the remaining time.
 *
 * @return the number of remaining ms to sleep if interrupted, -1 if there was an error, 0 otherwise.
 */
int wrapperSleepInterrupt(int ms, int interrupt) {
    int result;

    /* We want to use nanosleep if it is available, but make it possible for the
       user to build a version that uses usleep if they want.
       usleep does not behave nicely with signals thrown while sleeping.  This
       was the believed cause of a hang experienced on one Solaris system. */
#ifdef USE_USLEEP
    if (wrapperData && wrapperData->isSleepOutputEnabled) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("    Sleep: usleep %dms"), ms);
    }
    result = wrapperUsleep(ms * 1000); /* microseconds */
#else
    struct timespec ts;
    struct timespec tsRemaining;
 #ifdef HPUX
    int failed = FALSE;
 #endif

    if (ms >= 1000) {
        ts.tv_sec = ms / 1000;
        ts.tv_nsec = (ms % 1000) * 1000000; /* nanoseconds */
    } else {
        ts.tv_sec = 0;
        ts.tv_nsec = ms * 1000000; /* nanoseconds */
    }

    if (wrapperData && wrapperData->isSleepOutputEnabled) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("    Sleep: nanosleep %dms"), ms);
    }
 #ifdef HPUX
    /* On HPUX, there is an issue when two threads call nanosleep() at the same time.
     *  This happens for example when the timer thread calls wrapperSleep() while the main
     *  thread is waiting for the output of 'java -version'.
     *  According to the documentation, nanosleep() should be thread-safe, but the
     *  documentation also states that errno should be set when the function fails, which
     *  is not the case here (errno=0)! The implementation of nanosleep() may not be
     *  correct on this platform (not sure if it was fixed on later versions of the OS).
     *  To fix this issue, we can't really use a mutex because the timer thread would
     *  constantly block the main thread. Instead, we will try calling again nanosleep
     *  but we'll do it to only one time to avoid infinite loop. When this happens, the
     *  first call probably did not sleep at all (and even if it did, it is not sure
     *  whether the remaining time would be set correctly), so just sleep again the full
     *  amount of time on the second call. */
   tryagain:
 #endif
    errno = 0;
    while (((result = nanosleep(&ts, &tsRemaining)) == -1) && (errno == EINTR)) {
        if (interrupt) {
            return (tsRemaining.tv_sec * 1000) + (tsRemaining.tv_nsec / 1000000);
        }
        ts.tv_sec = tsRemaining.tv_sec;
        ts.tv_nsec = tsRemaining.tv_nsec;
    }

    if (result) {
        if (errno == EAGAIN) {
            /* On 64-bit AIX this happens once on shutdown. */
            if (wrapperData && wrapperData->isSleepOutputEnabled) {
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS,
                    TEXT("    Sleep: nanosleep unavailable"));
            }
            if (interrupt) {
                return (tsRemaining.tv_sec * 1000) + (tsRemaining.tv_nsec / 1000000);
            }
        } else {
 #ifdef HPUX
            if ((errno == 0) && !failed) {
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG,
                    TEXT("nanosleep(%dms) failed. %s. Trying again."), ms, getLastErrorText());
                failed = TRUE;
                goto tryagain;
            }
 #endif
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR,
                TEXT("nanosleep(%dms) failed. %s"), ms, getLastErrorText());
        }
    }
#endif

    if (wrapperData && wrapperData->isSleepOutputEnabled) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("    Sleep: awake"));
    }
    
    return result;
}

/**
 * Cause the current thread to sleep for the specified number of milliseconds.
 *  This function will not be interrupted by signals.
 *
 * @param ms Number of milliseconds to wait for.
 */
void wrapperSleep(int ms) {
    wrapperSleepInterrupt(ms, FALSE);
}

/**
 * Detaches the Java process so the Wrapper will if effect forget about it.
 */
void wrapperDetachJava() {
    wrapperSetJavaState(WRAPPER_JSTATE_DOWN_CLEAN, 0, -1);
    
    /* Leave the stdout/stderr pipe in pipedes alone so we grab any remaining output.
     *  They should have been redirected on the JVM side anyway. */
}


void wrapperDisposeJavaVersionCommand() {
    int i;

    if(wrapperData->jvmVersionCommand) {
        for (i = 0; wrapperData->jvmVersionCommand[i] != NULL; i++) {
            free(wrapperData->jvmVersionCommand[i]);
            wrapperData->jvmVersionCommand[i] = NULL;
        }
        free(wrapperData->jvmVersionCommand);
        wrapperData->jvmVersionCommand = NULL;
    }
}

/**
 * Build the command line used to get the Java version.
 *
 * @return TRUE if there were any problems.
 */
int wrapperBuildJavaVersionCommand() {
    TCHAR **strings;

    /* If this is not the first time through, then dispose the old command array */
    wrapperDisposeJavaVersionCommand();
    
    strings = malloc(sizeof(TCHAR*));
    if (!strings) {
        outOfMemory(TEXT("WBJVC1"), 1);
        return TRUE;
    }
    memset(strings, 0, sizeof(TCHAR *));
    
    if (wrapperBuildJavaCommandArrayJavaCommand(strings, 0, FALSE) < 0) {
        wrapperFreeStringArray(strings, 1);
        return TRUE;
    }
    
    if (wrapperResolveJavaVersionCommand(&(strings[0]))) {
        wrapperFreeStringArray(strings, 1);
        return TRUE;
    }

    /* Allocate memory to hold array of version command strings.  The array is itself NULL terminated */
    wrapperData->jvmVersionCommand = malloc(sizeof(TCHAR *) * (2 + 1));
    if (!wrapperData->jvmVersionCommand) {
        outOfMemory(TEXT("WBJVC"), 2);
        wrapperFreeStringArray(strings, 1);
        return TRUE;
    }
    memset(wrapperData->jvmVersionCommand, 0, sizeof(TCHAR *) * (2 + 1));
    /* Java Command */
    wrapperData->jvmVersionCommand[0] = malloc(sizeof(TCHAR) * (_tcslen(strings[0]) + 1));
    if (!wrapperData->jvmVersionCommand[0]) {
        outOfMemory(TEXT("WBJVC"), 3);
        wrapperFreeStringArray(strings, 1);
        return TRUE;
    }
    _tcsncpy(wrapperData->jvmVersionCommand[0], strings[0], _tcslen(strings[0]) + 1);
    /* -version */
    wrapperData->jvmVersionCommand[1] = malloc(sizeof(TCHAR) * (8 + 1));
    if (!wrapperData->jvmVersionCommand[1]) {
        outOfMemory(TEXT("WBJVC"), 4);
        wrapperFreeStringArray(strings, 1);
        return TRUE;
    }
    _tcsncpy(wrapperData->jvmVersionCommand[1], TEXT("-version"), 8 + 1);
    /* NULL */
    wrapperData->jvmVersionCommand[2] = NULL;
    
    wrapperFreeStringArray(strings, 1);
    
    return FALSE;
}

void wrapperDisposeJavaBootstrapCommand() {
    int i;

    if (wrapperData->jvmBootstrapCommand) {
        for (i = 0; wrapperData->jvmBootstrapCommand[i] != NULL; i++) {
            free(wrapperData->jvmBootstrapCommand[i]);
            wrapperData->jvmBootstrapCommand[i] = NULL;
        }
        free(wrapperData->jvmBootstrapCommand);
        wrapperData->jvmBootstrapCommand = NULL;
    }
}

int wrapperBuildJavaBootstrapCommand(int id, const TCHAR* entryPoint) {
    TCHAR **strings;
    int length, i;

    /* If this is not the first time through, then dispose the old command array */
    wrapperDisposeJavaBootstrapCommand();

    /* Build the Java Command Strings */
    strings = NULL;
    length = 0;
    if (wrapperBuildJavaBootstrapCommandArray(&strings, &length, id, entryPoint)) {
        wrapperFreeStringArray(strings, length);
        return TRUE;
    }

    /* Allocate memory to hold array of command strings.  The array is itself NULL terminated */
    wrapperData->jvmBootstrapCommand = malloc(sizeof(TCHAR *) * (length + 1));
    if (!wrapperData->jvmBootstrapCommand) {
        outOfMemory(TEXT("WBJBC"), 1);
        wrapperFreeStringArray(strings, length);
        return TRUE;
    }
    memset(wrapperData->jvmBootstrapCommand, 0, sizeof(TCHAR *) * (length + 1));

    for (i = 0; i < length; i++) {
        wrapperData->jvmBootstrapCommand[i] = malloc(sizeof(TCHAR) * (_tcslen(strings[i]) + 1));
        if (!wrapperData->jvmBootstrapCommand[i]) {
            outOfMemory(TEXT("WBJBC"), 2);
            wrapperFreeStringArray(strings, length);
            return TRUE;
        }
        _tcsncpy(wrapperData->jvmBootstrapCommand[i], strings[i], _tcslen(strings[i]) + 1);
    }
    wrapperData->jvmBootstrapCommand[i] = NULL;

    /* Free up the temporary command array */
    wrapperFreeStringArray(strings, length);

    return FALSE;
}

void wrapperDisposeJavaCommand() {
    int i;

    if (wrapperData->jvmCommand) {
        for (i = 0; wrapperData->jvmCommand[i] != NULL; i++) {
            free(wrapperData->jvmCommand[i]);
            wrapperData->jvmCommand[i] = NULL;
        }
        free(wrapperData->jvmCommand);
        wrapperData->jvmCommand = NULL;
    }
    if (wrapperData->jvmDryCommand) {
        /* The dry command shares the same array components as the Java command line, except for the '--dry-run' parameter in the second position. */
        free(wrapperData->jvmDryCommand[1]);
        wrapperData->jvmDryCommand[1] = NULL;
        free(wrapperData->jvmDryCommand);
        wrapperData->jvmDryCommand = NULL;
    }
}

/**
 * Build the java command line.
 *
 * @return TRUE if there were any problems.
 */
int wrapperBuildJavaCommand(int buildDryCommand) {
    TCHAR **strings;
    int length, i, j, k;
    int skip;

    /* If this is not the first time through, then dispose the old command arrays */
    wrapperDisposeJavaCommand();

    /* Build the Java Command Strings */
    strings = NULL;
    length = 0;
    if (wrapperBuildJavaCommandArray(&strings, &length, wrapperData->classpath)) {
        wrapperFreeStringArray(strings, length);
        return TRUE;
    }
    
    /* Allocate memory to hold array of command strings.  The array is itself NULL terminated */
    wrapperData->jvmCommand = malloc(sizeof(TCHAR *) * (length + 1));
    if (!wrapperData->jvmCommand) {
        outOfMemory(TEXT("WBJC"), 1);
        wrapperFreeStringArray(strings, length);
        return TRUE;
    }
    memset(wrapperData->jvmCommand, 0, sizeof(TCHAR *) * (length + 1));
    
    if (buildDryCommand) {
        /* Allocate memory to hold array of command strings.  The array is itself NULL terminated */
        wrapperData->jvmDryCommand = malloc(sizeof(TCHAR *) * (length - wrapperData->appOnlyAdditionalCount + 2));
        if (!wrapperData->jvmDryCommand) {
            outOfMemory(TEXT("WBJC"), 2);
            wrapperFreeStringArray(strings, length + 1);
            return TRUE;
        }
        memset(wrapperData->jvmDryCommand, 0, sizeof(TCHAR *) * (length - wrapperData->appOnlyAdditionalCount + 2));
    }

    k = 0;
    for (i = 0, j = 0; i < length; i++, j++) {
        wrapperData->jvmCommand[i] = malloc(sizeof(TCHAR) * (_tcslen(strings[i]) + 1));
        if (!wrapperData->jvmCommand[i]) {
            outOfMemory(TEXT("WBJC"), 3);
            wrapperFreeStringArray(strings, length);
            return TRUE;
        }
        _tcsncpy(wrapperData->jvmCommand[i], strings[i], _tcslen(strings[i]) + 1);
        wrapperData->jvmCommand[i] = wrapperPostProcessCommandElement(wrapperData->jvmCommand[i]);

        if (buildDryCommand) {
            if (j == 1) {
                wrapperData->jvmDryCommand[j] = malloc(sizeof(TCHAR) * (9 + 1));
                if (!wrapperData->jvmDryCommand[j]) {
                    outOfMemory(TEXT("WBJC"), 4);
                    wrapperFreeStringArray(strings, length);
                    return TRUE;
                }
                _tcsncpy(wrapperData->jvmDryCommand[j], TEXT("--dry-run"), 9 + 1);
                j++;
            } else {
                skip = FALSE;
                for (; (k < wrapperData->appOnlyAdditionalCount) && (wrapperData->appOnlyAdditionalIndexes[k] <= i); k++) {
                    if (wrapperData->appOnlyAdditionalIndexes[k] == i) {
                        /* this parameter must be skipped for the --dry-run command line */
                        skip = TRUE;
                        break;
                    }
                }
                if (skip) {
                    j--;
                    continue;
                }
            }
            wrapperData->jvmDryCommand[j] = wrapperData->jvmCommand[i];
        }
    }
    wrapperData->jvmCommand[i] = NULL;
    if (buildDryCommand) {
        wrapperData->jvmDryCommand[j] = NULL;
    }

    /* Free up the temporary command array */
    wrapperFreeStringArray(strings, length);

    return FALSE;
}

/**
 * Calculate the total length of the environment assuming that they are separated by spaces.
 */
size_t wrapperCalculateEnvironmentLength() {
    /* The compiler won't let us reference environ directly in the for loop on OSX because it is actually a function. */
    char **environment = environ;
    size_t i;
    size_t len;
    size_t lenTotal;
    
    i = 0;
    lenTotal = 0;
    while (environment[i]) {
        /* All we need is the length so we don't actually have to convert them. */
        len = mbstowcs(NULL, environment[i], MBSTOWCS_QUERY_LENGTH);
        if (len == (size_t)-1) {
            /* Invalid string.  Skip. */
        } else {
            /* Add length of variable + null + pointer to next element */
            lenTotal += len + 1 + sizeof(char *);
        }
        i++;
    }
    /* Null termination of the list. */
    lenTotal += sizeof(char *) + sizeof(char *);
    
    return lenTotal;
}

/**
 * Launch a JVM and collect the pid.
 *
 * @return TRUE if there were any problems, FALSE otherwise.
 */
int wrapperLaunchJvm(TCHAR** command, int isApp, pid_t *pidPtr) {
    int i;
    pid_t proc;
    int execErrno;
    size_t lenCmd;
    size_t lenEnv;
    int useStdin;
    int newGroup;

    /* Create a single pipe for stdout and stderr (they will be merged). */
    if (pipe(pipedes) < 0) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL,
                   TEXT("Could not init %s pipe: %s"), TEXT("stdout/stderr"), getLastErrorText());
        return TRUE;
    }

    if (isApp) {
        /* Main Java process. */
        newGroup = wrapperData->javaNewProcessGroup;

        /* stdin is sent to the foreground process group (the group of the Wrapper).
         *  - If the Java process is started in the same group, then it will also receive stdin, so we don't need to redirect it.
         *  - If the Java process is started in a new group, redirection is needed (we want to keep the Wrapper running in the foreground process group to catch signals). */
        if (newGroup && !wrapperData->disableConsoleInput) {
            if (pipe(pipeind) < 0) {
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL,
                           TEXT("Could not init %s pipe: %s"), TEXT("stdin"), getLastErrorText());
                return TRUE;
            }
            /* When Java shuts down, the pipe is disconnected and a SIGPIPE signal is sent.  Ignore it as it would cause the Wrapper process to exit. */
            signal(SIGPIPE, SIG_IGN);
            useStdin = TRUE;
        } else {
            useStdin = FALSE;
        }
    } else {
        /* Java queries ('-version', bootstrap, '--dry-run') should always be started in a new process group otherwise they would
         *  catch CTRL-C signals and exit on their own without us being able to tell if it's a crash or an interruption. */
        newGroup = TRUE;
        useStdin = FALSE;
    }

    /* Again make sure the log file is closed before forking. */
    setLogfileAutoClose(TRUE);
    closeLogfile();

    /* Reset the log duration so we get new counts from the time the JVM is launched. */
    resetDuration();
    
    /* Flush stdout and stderr before forking. Otherwise, there is a risk that the Wrapper output will be duplicated in the child output. */
    fflush(stdout);
    fflush(stderr);

    /* Fork off the child. */
    proc = fork();

    if (proc == -1) {
        /* Fork failed. */

        /* Restore the auto close flag. */
        setLogfileAutoClose(wrapperData->logfileCloseTimeout == 0);

        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL,
                   TEXT("Could not spawn JVM process: %s"), getLastErrorText());

        /* The fork failed so there is no child side.  Close the pipes so we don't attempt to read them later. */
        close(pipedes[PIPE_READ_END]);
        pipedes[PIPE_READ_END] = -1;
        close(pipedes[PIPE_WRITE_END]);
        pipedes[PIPE_WRITE_END] = -1;
        if (useStdin) {
            close(pipeind[PIPE_READ_END]);
            pipeind[PIPE_READ_END] = -1;
            close(pipeind[PIPE_WRITE_END]);
            pipeind[PIPE_WRITE_END] = -1;
        }
        if (isApp) {
            if (protocolPipeInFd[PIPE_READ_END] != -1) {
                close(protocolPipeInFd[PIPE_READ_END]);
                protocolPipeInFd[PIPE_READ_END] = -1;
            }
            if (protocolPipeInFd[PIPE_WRITE_END] != -1) {
                close(protocolPipeInFd[PIPE_WRITE_END]);
                protocolPipeInFd[PIPE_WRITE_END] = -1;
            }
            if (protocolPipeOuFd[PIPE_READ_END] != -1) {
                close(protocolPipeOuFd[PIPE_READ_END]);
                protocolPipeOuFd[PIPE_READ_END] = -1;
            }
            if (protocolPipeOuFd[PIPE_WRITE_END] != -1) {
                close(protocolPipeOuFd[PIPE_WRITE_END]);
                protocolPipeOuFd[PIPE_WRITE_END] = -1;
            }
        }

        return TRUE;
    } else if (proc == 0) {
        /* We are the child side. */

        /* Set the umask of the JVM */
        umask(wrapperData->javaUmask);

        /* The logging code causes some log corruption if logging is called from the
         *  child of a fork.  Not sure exactly why but most likely because the forked
         *  child receives a copy of the mutex and thus synchronization is not working.
         * It is ok to log errors in here, but avoid output otherwise.
         * TODO: Figure out a way to fix this.  Maybe using shared memory? */

        close(pipedes[PIPE_READ_END]);
        pipedes[PIPE_READ_END] = -1;

        /* Send output to the pipe by duplicating the pipe fd and setting the copy as the stdout fd. */
        if (dup2(pipedes[PIPE_WRITE_END], STDOUT_FILENO) < 0) {
            /* This process needs to end (no meaning to log an error without stdout/stderr). */
            exit(wrapperData->errorExitCode);
            close(pipedes[PIPE_WRITE_END]);
            pipedes[PIPE_WRITE_END] = -1;
            return TRUE; /* Will not get here. */
        }

        /* Send errors to the pipe by duplicating the pipe fd and setting the copy as the stderr fd. */
        if (dup2(pipedes[PIPE_WRITE_END], STDERR_FILENO) < 0) {
            /* This process needs to end (no meaning to log an error without stdout/stderr). */
            exit(wrapperData->errorExitCode);
            close(pipedes[PIPE_WRITE_END]);
            pipedes[PIPE_WRITE_END] = -1;
            return TRUE; /* Will not get here. */
        }
        
        close(pipedes[PIPE_WRITE_END]);
        pipedes[PIPE_WRITE_END] = -1;

        if (useStdin) {
            close(pipeind[PIPE_WRITE_END]);
            pipeind[PIPE_WRITE_END] = -1;

            /* Send input to the pipe by duplicating the pipe fd and setting the copy as the stdin fd. */
            if (dup2(pipeind[PIPE_READ_END], STDIN_FILENO) < 0) {
                /* This process needs to end. */
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT("%sUnable to set JVM's stdin: %s"), LOG_FORK_MARKER, getLastErrorText());
                close(pipeind[PIPE_READ_END]);
                pipeind[PIPE_READ_END] = -1;
                exit(wrapperData->errorExitCode);
                return TRUE; /* Will not get here. */
            }

            close(pipeind[PIPE_READ_END]);
            pipeind[PIPE_READ_END] = -1;
        }

        if (isApp) {
            if (protocolPipeInFd[PIPE_READ_END] != -1) {
                close(protocolPipeInFd[PIPE_READ_END]);
                protocolPipeInFd[PIPE_READ_END] = -1;
            }
            if (protocolPipeOuFd[PIPE_WRITE_END] != -1) {
                close(protocolPipeOuFd[PIPE_WRITE_END]);
                protocolPipeOuFd[PIPE_WRITE_END] = -1;
            }
        }

        if (newGroup) {
            /* Java should be started in a new process group. */
            if (setpgid(0, 0)) {
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT("%sFailed to set Java process as group leader. %s"), LOG_FORK_MARKER, getLastErrorText());
            }
        }
        
        /* Child process: execute the JVM. */
        _texecvp(command[0], command);
        execErrno = errno;

        /* We reached this point...meaning we were unable to start. */
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR,
            TEXT("%sUnable to start JVM: %s (%d)"), LOG_FORK_MARKER, getLastErrorText(), execErrno);
        if (execErrno == E2BIG) {
            /* Command line too long. */
            /* Calculate the total length of the command line. */
            lenCmd = 0;
            for (i = 0; command[i] != NULL; i++) {
                lenCmd += _tcslen(command[i]) + 1;
            }
            lenEnv = wrapperCalculateEnvironmentLength();
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT("%s  The generated command line plus the environment was larger than the maximum allowed."), LOG_FORK_MARKER);
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT("%s  The current length is %d bytes of which %d is the command line, and %d is the environment."), LOG_FORK_MARKER, lenCmd + lenEnv + 1, lenCmd, lenEnv); 
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT("%s  It is not possible to calculate an exact maximum length as it depends on a number of factors for each system."), LOG_FORK_MARKER);

            /* TODO: Figure out a way to inform the Wrapper not to restart and try again as repeatedly doing this is meaningless. */
        }

        if (wrapperData->isAdviserEnabled) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ADVICE, TEXT("%s"), LOG_FORK_MARKER );
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ADVICE,
                TEXT("%s------------------------------------------------------------------------"), LOG_FORK_MARKER );
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ADVICE,
                TEXT("%sAdvice:"), LOG_FORK_MARKER );
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ADVICE,
                TEXT("%sUsually when the Wrapper fails to start the JVM process, it is because"), LOG_FORK_MARKER );
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ADVICE,
                TEXT("%sof a problem with the value of the configured Java command.  Currently:"), LOG_FORK_MARKER );
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ADVICE,
                TEXT("%swrapper.java.command=%s"), LOG_FORK_MARKER, getStringProperty(properties, TEXT("wrapper.java.command"), TEXT("java")));
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ADVICE,
                TEXT("%sPlease make sure that the PATH or any other referenced environment"), LOG_FORK_MARKER );
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ADVICE,
                TEXT("%svariables are correctly defined for the current environment."), LOG_FORK_MARKER );
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ADVICE,
                TEXT("%s------------------------------------------------------------------------"), LOG_FORK_MARKER );
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ADVICE, TEXT("%s"), LOG_FORK_MARKER );
        }

        /* This process needs to end. */
        if (!isApp) {
            /* Set an exit code to distinguish cases where we failed to execute the java command. */
            exit(127);
        } else {
            exit(wrapperData->errorExitCode);
        }
        return TRUE; /* Will not get here. */
    } else {
        /* We are the parent side and need to assume that at this point the JVM is up. */
        *pidPtr = proc;
        
        /* Close the write end as it is not used. */
        close(pipedes[PIPE_WRITE_END]);
        pipedes[PIPE_WRITE_END] = -1;
        if (useStdin) {
            /* Close the read end as it is not used. */
            close(pipeind[PIPE_READ_END]);
            pipeind[PIPE_READ_END] = -1;
        }
        if (isApp) {
            if (protocolPipeInFd[PIPE_WRITE_END] != -1) {
                close(protocolPipeInFd[PIPE_WRITE_END]);
                protocolPipeInFd[PIPE_WRITE_END] = -1;
            }
            if (protocolPipeOuFd[PIPE_READ_END] != -1) {
                close(protocolPipeOuFd[PIPE_READ_END]);
                protocolPipeOuFd[PIPE_READ_END] = -1;
            }
        }

        /* The pipedes & pipeind arrays are global so do not close the other ends of the pipes. */

        /* Restore the auto close flag. */
        setLogfileAutoClose(wrapperData->logfileCloseTimeout == 0);

        /* Mark our sides of the pipes so that they won't block
         * and will close on exec, so new children won't see them. */
        if (fcntl(pipedes[PIPE_READ_END], F_SETFL, O_NONBLOCK) < 0) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR,
                TEXT("Failed to set JVM output handle to non blocking mode: %s (%d)"),
                getLastErrorText(), errno);
        }
        if (fcntl(pipedes[PIPE_READ_END], F_SETFD, FD_CLOEXEC) < 0) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR,
                TEXT("Failed to set JVM output handle to close on JVM exit: %s (%d)"),
                getLastErrorText(), errno);
        }
        if (useStdin) {
            /* Mark our side of the pipe so that it will block on writing if the pipe is full
             * and will close on exec, so new children won't see it. */
            if (fcntl(pipeind[PIPE_WRITE_END], F_SETFL, O_SYNC) < 0) {
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR,
                    TEXT("Failed to set synchronized I/O file integrity completion for JVM input handle: %s (%d)"),
                    getLastErrorText(), errno);
            }
            if (fcntl(pipeind[PIPE_WRITE_END], F_SETFD, FD_CLOEXEC) < 0) {
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR,
                    TEXT("Failed to set JVM input handle to close on JVM exit: %s (%d)"),
                    getLastErrorText(), errno);
            }
        }
        if (isApp) {
            if (protocolPipeInFd[PIPE_READ_END] != -1) {
                if (fcntl(protocolPipeInFd[PIPE_READ_END], F_SETFL, O_NONBLOCK) < 0) {
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR,
                        TEXT("Failed to set backend input handle to non blocking mode: %s (%d)"),
                        getLastErrorText(), errno);
                }
                if (fcntl(protocolPipeInFd[PIPE_READ_END], F_SETFD, FD_CLOEXEC) < 0) {
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR,
                        TEXT("Failed to set backend input handle to close on JVM exit: %s (%d)"),
                        getLastErrorText(), errno);
                }
            }
            if (protocolPipeOuFd[PIPE_WRITE_END] != -1) {
                if (fcntl(protocolPipeOuFd[PIPE_WRITE_END], F_SETFL, O_NONBLOCK) < 0) {
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR,
                        TEXT("Failed to set backend output handle to non blocking mode: %s (%d)"),
                        getLastErrorText(), errno);
                }
                if (fcntl(protocolPipeOuFd[PIPE_WRITE_END], F_SETFD, FD_CLOEXEC) < 0) {
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR,
                        TEXT("Failed to set backend output handle to close on JVM exit: %s (%d)"),
                        getLastErrorText(), errno);
                }
            }
        }
        return FALSE;
    }
}

static int wrapperKillJavaQuery(pid_t pid, const TCHAR* commandDesc) {
    int ret;               /* result of waitpid */
    int status;            /* status of child process */

    if ((kill(pid, SIGKILL) != 0) && (errno != ESRCH)) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT("Child process: %s: kill failed - %s"), commandDesc, getLastErrorText());
        return TRUE;
    }
    /* The process is now killed.  We need to call waitpid() again otherwise it would show as <defunct>. */
    ret = waitpid(pid, &status, 0);
    if (ret == 0) {
        /* Should never happen. */
    } else if (ret < 0) {
        /* Not critical, but report. */
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT("Child process: %s: wait after kill failed - %s"), commandDesc, getLastErrorText());
    }
    return FALSE;
}

#ifdef LINUX
/**
 * Check if the glibc version of the user is upper to given numbers. 
 */
int wrapperAssertGlibcUser(unsigned int maj, unsigned int min, unsigned int rev) {
    unsigned int vmaj = 0;
    unsigned int vmin = 0;
    unsigned int vrev = 0;
    const char* version = gnu_get_libc_version();

    /* At least major.minor should be parsed, so the result must be greater than 2. Errors would be EOF(-1). */
    if (sscanf(version, "%u.%u.%u", &vmaj, &vmin, &vrev) < 2) {
        return FALSE;
    }

    return ((vmaj == maj && vmin == min &&  vrev >= rev) ||
            (vmaj == maj && vmin > min) ||
            (vmaj > maj)) ? TRUE : FALSE;
}
#endif

#if defined(LINUX) || defined(SOLARIS)
 #define DEFAULT_PIPE_CAPACITY   65536
#else
 #define DEFAULT_PIPE_CAPACITY   16384
#endif

static int isReadPipeFull() {
    int nBytes = 0;
    int size;

    if (pipedes[PIPE_READ_END] == -1) {
        /* The child is not up. */
        return FALSE;
    }

#ifdef LINUX
 #ifndef F_GETPIPE_SZ
  #define F_GETPIPE_SZ 1032
 #endif

    if (wrapperAssertGlibcUser(2, 13, 0)) {
        size = fcntl(pipedes[0], F_GETPIPE_SZ);
        if (size == -1) {
            /* Failed to retrieve the size of the pipe. */
            size =  DEFAULT_PIPE_CAPACITY;
        }
    } else {
        size =  DEFAULT_PIPE_CAPACITY;
    }
#else
    /* There is no public API to determine the capacity of a pipe buffer.
     *  Instead use the default pipe capacity. */
    size =  DEFAULT_PIPE_CAPACITY;
#endif

    if (ioctl(pipedes[PIPE_READ_END], FIONREAD, &nBytes) == -1) {
        /* Failed to get the number of byte in the input buffer. */
        return FALSE;
    }

    if (nBytes + READ_BUFFER_BLOCK_SIZE >= size) {
        /* Pipe is full. */
        return TRUE;
    }

    return FALSE;
}

int wrapperQueryJava(TCHAR** command, const TCHAR* commandDesc, int useLocalEncoding, int blockTimeout, int logExitWithError, int* pExitCode) {
    int ret = 0;           /* result of waitpid, will be set on the first loop but initialize to 0 to avoid warning reported by some compilers */
    int status;            /* status of child process */
    pid_t pid;             /* pid of the child process */
    int skipWait = FALSE;
    TICKS start;
    int sleepMs;
    int stepTimeout = 100;
    int steps = 0;
    int step;
    int maxStepsBeforeRead;
    int done = FALSE;
    int result = JAVA_PROC_COMPLETED;

    wrapperData->jvmQueryEvaluated = FALSE;
    wrapperData->jvmQueryCompleted = FALSE;

    if (useLocalEncoding) {
        /* Force using the encoding of the current locale to read the output. */
        resetJvmOutputEncoding(FALSE);
    }
    
    if (wrapperLaunchJvm(command, FALSE, &pid)) {
        return JAVA_PROC_LAUNCH_FAILED;
    }

    wrapperData->javaQueryPID = pid;

    if (wrapperData->isDebugging) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("JVM (%s) started (PID=%d)"), commandDesc, wrapperData->javaQueryPID);
    }

    if (blockTimeout <= 0) {
        /* Wait indefinitely. */
    } else {
        steps = blockTimeout / stepTimeout;
    }

    /* Ideally, we'd like to wait for the process to complete before reading its output.
     *  This way, we can adjust the log level based on the error code.
     *  wrapperData->javaQueryEvaluationTimeout is the timeout in seconds before
     *  evaluating the output. Once the necessary information is found, the wrapper will
     *  kill the child process after an additional 2 seconds. Otherwise, it waits for
     *  the process to terminate (up to the number of seconds set with blockTimeout).
     *  In most cases, the process will complete almost immediately.
     *  Note: The process could also block if the pipe reaches its capacity. This can
     *        happen with large output, for example, when using debugging options.
     *        If this happens, the output will be printed immediately. */
    maxStepsBeforeRead = wrapperData->javaQueryEvaluationTimeout / stepTimeout;

    step = 0;
    do {
        if (skipWait) {
            skipWait = FALSE;
        } else {
            ret = waitpid(pid, &status, WNOHANG);
        }
        if (ret > 0) {
            /* Process completed - we know that nothing more can be written to stdout/stderr. */
            wrapperData->jvmQueryCompleted = TRUE;
            if (WIFEXITED(status)) {
                *pExitCode = WEXITSTATUS(status);
            } else {
                /* This should never happen unless the process crashed or was interrupted by a signal (note that
                 *  java queries are launched as group leaders, so they don't catch signals sent to the Wrapper).
                 *  Even if that would be the case, it doesn't hurt to try to read and parse the output. */
                while (wrapperReadChildOutput(250)) {}; /* keep process output before wrapper messages */
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT("Child process: %s: did not exit normally."), commandDesc);
                *pExitCode = 0;
            }
            wrapperData->jvmQueryExitCode = *pExitCode;
            if (*pExitCode == 0) {
                while (wrapperReadChildOutput(250)) {};
            } else {
                /* The output of the forked child may contain:
                 *  - the output of command that failed (if any), which may give us a clue of what the problem is.
                 *  - messages of the Wrapper. Those should start with 'LOG_FORK_MARKER' so that log_printf format them as if they were printed by the parent. */
                wrapperReadAllChildOutputAfterFailure();

                if (*pExitCode == 127) {
                    /* We called exit(127) in the child process, which means the java command could not execute normally.
                     *  This is similar to Windows when wrapperLaunchJvm() returns TRUE.
                     *  Most probably we won't get any output from the Java command itself, but we still need to print the Wrapper messages of the forked child. */
                    result = JAVA_PROC_LAUNCH_FAILED;
                } else {
                    /* The child process terminated (within '_texecvp') with an 'exit'. */
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT("Child process: %s: terminated with an exit code of %d."), commandDesc, *pExitCode);
                }
            }
        } else {
            if (ret == 0) {
                if ((step < steps) || (blockTimeout <= 0)) {
                    wrapperSetUptime(wrapperGetTicks(), NULL);

                    if (wrapperMaintainSignals()) {
                        wrapperKillJavaQuery(pid, commandDesc);
                        while (wrapperReadChildOutput(250)) { }
                        result = JAVA_PROC_INTERRUPTED;
                        break;
                    }

                    if ((step >= maxStepsBeforeRead) || isReadPipeFull()) {
                        /* Avoid calling isReadPipeFull() again */
                        maxStepsBeforeRead = step;

                        start = wrapperGetTicks();
                        if (wrapperReadChildOutput(stepTimeout)) {
                            skipWait = TRUE;
                        } else {
                            sleepMs = stepTimeout - (wrapperGetTickAgeTicks(start, wrapperGetTicks()) * WRAPPER_TICK_MS);
                            if (sleepMs > 0) {
                                wrapperSleep(sleepMs);
                            }
                        }
                        if (blockTimeout > 0) {
                            step++;
                        }
                        if ((wrapperData->jvmQueryEvaluated) && (steps > step + (2000 / stepTimeout))) {
                            /* The query has been evaluated. Allow 2 additional seconds for the process to exit. */
                            steps = step + (2000 / stepTimeout);
                        }
                    } else {
                        wrapperSleep(stepTimeout);
                        step++;
                    }
                    continue;
                }
                /* Timed out. */
                log_printf(WRAPPER_SOURCE_WRAPPER, wrapperData->jvmQueryEvaluated ? LEVEL_DEBUG : LEVEL_ERROR, TEXT("Child process: %s: timed out"), commandDesc);
            } else {
                /* Wait failed. */
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT("Child process: %s: wait failed - %s"), commandDesc, getLastErrorText());
            }
            if (wrapperKillJavaQuery(pid, commandDesc)) {
                result = JAVA_PROC_KILL_FAILED;
            } else if (wrapperData->jvmQueryEvaluated) {
                /* The process timed out and was killed, but we collected all the required information, so continue. */
                result = JAVA_PROC_COMPLETED;
            } else {
                result = JAVA_PROC_WAIT_FAILED;
            }
        }
        done = TRUE; /* loops with 'continue' will not pass by here */
    } while (!done);

    return result;
}

#define ALL_ARGS                        0
#define ALL_ARGS_EXCEPT_BACKEND_PROPS   1
#define BACKEND_PROPS                   2

static size_t quoteArg(TCHAR* buffer, const TCHAR* val, size_t valLen, const TCHAR* specialChars) {
    TCHAR* ptr1;
    TCHAR* ptr2;
    int i, out;
    int quoteIndex = -1;

    if (valLen == 0) {
        quoteIndex = 0;
    } else {
        ptr1 = _tcspbrk(val, specialChars);
        if (ptr1) {
            ptr2 = _tcschr(val, TEXT('='));
            if (ptr2 && (ptr1 > ptr2)) {
                quoteIndex = (int)(ptr2 - val + 1);
            } else {
                quoteIndex = 0;
            }
        }
    }

    if (quoteIndex == -1) {
        if (buffer) {
            _tcsncpy(buffer, val, valLen);
        }
        out = valLen;
    } else {
        for (i = 0, out = 0; i < quoteIndex; i++, out++) {
            if (buffer) {
                buffer[out] = val[i];
            }
        }
        
        /* Opening quote */
        if (buffer) {
            buffer[out] = TEXT('\'');
        }
        out++;

        for (; i < valLen; i++, out++) {
            if (val[i] == TEXT('\'')) {
                if (buffer) {
                    buffer[out++] = TEXT('\''); /* close the quote */
                    buffer[out++] = TEXT('\\'); /* escape char */
                    buffer[out++] = TEXT('\''); /* escaped quote */
                    buffer[out] = TEXT('\''); /* reopen the quote */
                } else {
                    out += 3;
                }
            }
            if (buffer) {
                buffer[out] = val[i];
            }
        }

        /* Closing quote */
        if (buffer) {
            buffer[out] = TEXT('\'');
        }
        out++;
    }
    return out;
}

static size_t buildJavaCommandLine(JAVA_COMMAND_TYPE command, TCHAR* buffer, size_t len, int target) {
    const TCHAR* specialChars = TEXT(" \t\\*$|&();><!?\"'#");
    size_t len2 = 0;
    int i = 0;

    /* First, copy the command. */
    if (target != BACKEND_PROPS) {
        len2 += quoteArg(buffer, command[0], _tcslen(command[0]), specialChars);
        i++;
    }

    /* Copy the arguments. */
    for (; command[i] != NULL; i++) {
        if ((target == ALL_ARGS) ||
           (((target == ALL_ARGS_EXCEPT_BACKEND_PROPS) && !isBackendProperty(command[i]))) ||
           (((target == BACKEND_PROPS) && isBackendProperty(command[i])))) {

            if (buffer) {
                buffer[len2] = TEXT(' ');
            }
            len2++;

            if (buffer) {
                len2 += quoteArg(buffer + len2, command[i], _tcslen(command[i]), specialChars);
            } else {
                len2 += quoteArg(NULL, command[i], _tcslen(command[i]), specialChars);
            }
        }
    }
    if (buffer) {
        buffer[len2] = 0;
    }
    len2++;

    return len2;
}

void printJavaCommand(JAVA_COMMAND_TYPE command, int loglevel, int hideBackendProps) {
    int i;
    TCHAR* buffer;
    size_t len;
    int target;

    if (wrapperData->jvmCommandPrintFormat == COMMAND_FORMAT_ARRAY) {
        for (i = 0; command[i] != NULL; i++) {
            if (!hideBackendProps || !isBackendProperty(command[i])) {
                log_printf(WRAPPER_SOURCE_WRAPPER, loglevel, TEXT("  Command[%d] : %s"), i, command[i]);
            }
        }
    } else {
        target = hideBackendProps ? ALL_ARGS_EXCEPT_BACKEND_PROPS : ALL_ARGS;
        len = buildJavaCommandLine(command, NULL, 0, target);
        if (len > 0) {
            buffer = malloc(sizeof(TCHAR) * len);
            if (!buffer) {
                outOfMemory(TEXT("PJC"), 1);
            } else {
                buildJavaCommandLine(command, buffer, len, target);
                log_printf(WRAPPER_SOURCE_WRAPPER, loglevel, TEXT("  Command: %s"), buffer);
                free(buffer);
            }
        }
    }
}

/**
 * Launches a JVM process and stores it internally.
 *
 * @return TRUE if there were any problems.  When this happens the Wrapper will not try to restart.
 */
int wrapperLaunchJavaApp() {
    static int javaIOThreadSet = FALSE;
    TCHAR* backendProps;
    size_t backendPropsLen;
    pid_t pid;

    wrapperData->jvmCallType = WRAPPER_JVM_APP;
    wrapperData->jvmDefaultLogLevel = LEVEL_INFO;

    /* Update the CLASSPATH in the environment if requested so the JVM can access it. */ 
    if (wrapperData->environmentClasspath) {
        setEnv(TEXT("CLASSPATH"), wrapperData->classpath, ENV_SOURCE_APPLICATION);
    }
    
    /* Log the application java command line */
    if (wrapperData->commandLogLevel != LEVEL_NONE) {
        log_printf(WRAPPER_SOURCE_WRAPPER, wrapperData->commandLogLevel, TEXT("Java Command Line:"));
        printJavaCommand(wrapperData->jvmCommand, wrapperData->commandLogLevel, !wrapperData->jvmCommandShowBackendProps);

        if (wrapperData->environmentClasspath) {
            log_printf(WRAPPER_SOURCE_WRAPPER, wrapperData->commandLogLevel,
                TEXT("  Classpath in Environment : %s"), wrapperData->classpath);
        }
        if (wrapperData->isDebugging && !wrapperData->jvmCommandShowBackendProps) {
            backendPropsLen = buildJavaCommandLine(wrapperData->jvmCommand, NULL, 0, BACKEND_PROPS);
            if (backendPropsLen > 0) {
                backendProps = malloc(sizeof(TCHAR) * backendPropsLen);
                if (!backendProps) {
                    outOfMemory(TEXT("WLJA"), 1);
                    return TRUE;
                } else {
                    buildJavaCommandLine(wrapperData->jvmCommand, backendProps, backendPropsLen, BACKEND_PROPS);
                    /* Note: the list starts with a space. */
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("  Backend properties:%s"), backendProps);
                    free(backendProps);
                }
            }
        }
    }
    
    if (wrapperData->runWithoutJVM) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS,
            TEXT("Not launching a JVM because %s was set to TRUE."), TEXT("wrapper.test.no_jvm"));
        wrapperData->exitCode = 0;
        return TRUE;
    }
    
    if (wrapperData->useJavaIOThread) {
        /* Create and initialize a javaIO thread. */
        if (!javaIOThreadSet) {
            if (initializeJavaIO()) {
                return TRUE;
            }
            javaIOThreadSet = TRUE;
        }
    }
    
    /* Now launch the JVM process. */
    if (wrapperLaunchJvm(wrapperData->jvmCommand, TRUE, &pid)) {
        wrapperData->exitCode = wrapperData->errorExitCode;
        return TRUE;
    }
    
    /* Reset the exit code when we launch a new JVM. */
    wrapperData->exitCode = 0;
    
    /* Reset the stopped flag. */
    wrapperData->jvmStopped = FALSE;
    
    /* We keep a reference to the process id. */
    wrapperData->javaPID = pid;

    /* Log the PID of the new JVM. */
    if (wrapperData->pidLogLevel != LEVEL_NONE) {
        log_printf(WRAPPER_SOURCE_WRAPPER, wrapperData->pidLogLevel, TEXT("JVM started (PID=%d)"), wrapperData->javaPID);
    }

    /* If a java pid filename is specified then write the pid of the java process. */
    if (wrapperData->javaPidFilename) {
        if (writePidFile(wrapperData->javaPidFilename, wrapperData->javaPID, wrapperData->javaPidFileUmask, wrapperData->javaPidFileGroup)) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN,
                TEXT("Unable to write the Java PID file: %s"), wrapperData->javaPidFilename);
        }
    }

    /* If a java id filename is specified then write the Id of the java process. */
    if (wrapperData->javaIdFilename) {
        if (writePidFile(wrapperData->javaIdFilename, wrapperData->jvmRestarts, wrapperData->javaIdFileUmask, wrapperData->javaIdFileGroup)) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN,
                TEXT("Unable to write the Java Id file: %s"), wrapperData->javaIdFilename);
        }
    }

    return FALSE;
}

/**
 * Returns a tick count that can be used in combination with the
 *  wrapperGetTickAgeSeconds() function to perform time keeping.
 */
TICKS wrapperGetTicks() {
    TICKS ticks;
    
    if (wrapperData->useSystemTime) {
        /* We want to return a tick count that is based on the current system time. */
        ticks = wrapperGetSystemTicks();

    } else {
        /* Lock the tick mutex whenever the "timerTicks" variable is accessed. */
        if (wrapperData->useTickMutex && wrapperLockTickMutex()) {
            return 0;
        }
        
        /* Return a snapshot of the current tick count. */
        ticks = timerTicks;
        
        if (wrapperData->useTickMutex && wrapperReleaseTickMutex()) {
            return 0;
        }
    }
    
    return ticks;
}

/**
 * Simple function to check the status of the JVM process without calling wrapperJVMProcessExited().
 *  This function calls waitpid() with no WNOHANG. If the process was in a zombie state, this will
 *  cause to dispose the pid, which will invalidate wrapperCheckAndUpdateProcessStatus(). In such case,
 *  the status of waitpid() is kept for later processing by wrapperCheckAndUpdateProcessStatus().
 *
 * Returns WRAPPER_PROCESS_UP, WRAPPER_PROCESS_DOWN or WRAPPER_PROCESS_UNKNOWN.
 */
int wrapperQuickCheckJavaProcessStatus() {
    int retval;
    int status;

    if (wrapperData->javaPID <= 0) {
        return WRAPPER_PROCESS_DOWN;
    }

    /* Note: This call doesn't need WUNTRACED. Setting it only adds a case where waitpid() will return the PID of
     *       the child process if it was put into a "stopped" state by delivery of a signal. This case is covered by
     *       wrapperCheckAndUpdateProcessStatus() and not affected by this call, as javaPidUpdateStatus won't be set. */
    retval = waitpid(wrapperData->javaPID, &status, WNOHANG);

    if (retval == 0) {
        return WRAPPER_PROCESS_UP;
    } else if (retval < 0) {
        return WRAPPER_PROCESS_DOWN;
    } else {
        wrapperData->javaPidUpdateStatus = TRUE;
        wrapperData->javaPidWaitStatus = status;

        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            return WRAPPER_PROCESS_DOWN;
        } else if (WIFCONTINUED(status)) {
            return WRAPPER_PROCESS_UP;
        } else {
            return WRAPPER_PROCESS_UNKNOWN;
        }
    }
}

/**
 * Checks on the status of the JVM Process.
 *  ATTENTION: This function is called by several state functions assuming that any state after LAUNCHING
 *             will be updated if WRAPPER_PROCESS_DOWN is returned. In such case, wrapperData->javaPID
 *             should never be 0 otherwise the Wrapper would go into infinite looping.
 *
 * @param nowTicks The current TICKS.
 * @param childContinued True if called in response to having received a CLD_CONTINUED signal, always FALSE and ignored on Windows.
 *
 * Returns WRAPPER_PROCESS_UP or WRAPPER_PROCESS_DOWN
 */
int wrapperCheckAndUpdateProcessStatus(TICKS nowTicks, int childContinued) {
    int retval;
    int status;
    int exitCode;
    int res;
    
    if (wrapperData->javaPID <= 0) {
        /* We do not think that a JVM is currently running so return that it is down.
         * If we call waitpid with 0, it will wait for any child and cause problems with the event commands. */
        return WRAPPER_PROCESS_DOWN;
    }

    if (wrapperData->javaPidUpdateStatus) {
        /* The pid was already waited within wrapperQuickCheckJavaProcessStatus() but that was a silent call and we still need to update the process status. */
        retval = wrapperData->javaPID;
        status = wrapperData->javaPidWaitStatus;
        wrapperData->javaPidUpdateStatus = FALSE;
        wrapperData->javaPidWaitStatus = 0;
    } else {
        retval = waitpid(wrapperData->javaPID, &status, WNOHANG | WUNTRACED);
    }

    if (retval == 0) {
        /* Up and running. */
        if (childContinued && wrapperData->jvmStopped) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("JVM process was continued."));
            wrapperData->jvmStopped = FALSE;
        }
        res = WRAPPER_PROCESS_UP;
    } else if (retval < 0) {
        if (errno == ECHILD) {
            if ((wrapperData->jState == WRAPPER_JSTATE_DOWN_CHECK) ||
                (wrapperData->jState == WRAPPER_JSTATE_DOWN_FLUSH_STDIN) ||
                (wrapperData->jState == WRAPPER_JSTATE_DOWN_FLUSH) ||
                (wrapperData->jState == WRAPPER_JSTATE_DOWN_CLEAN) ||
                (wrapperData->jState == WRAPPER_JSTATE_STOPPED)) {
                res = WRAPPER_PROCESS_DOWN;
                wrapperJVMProcessExited(nowTicks, 0);
                return res;
            } else {
                /* Process is gone.  Happens after a SIGCHLD is handled. Normal. */
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("JVM process is gone."));
            }
        } else {
            /* Error requesting the status. */
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN, TEXT("Unable to request JVM process status: %s"), getLastErrorText());
        }
        exitCode = wrapperData->errorExitCode;
        res = WRAPPER_PROCESS_DOWN;
        wrapperJVMProcessExited(nowTicks, exitCode);
    } else {
#ifdef _DEBUG
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("  WIFEXITED=%d"), WIFEXITED(status));
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("  WIFSTOPPED=%d"), WIFSTOPPED(status));
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("  WIFSIGNALED=%d"), WIFSIGNALED(status));
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("  WTERMSIG=%d"), WTERMSIG(status));
#endif

        /* Get the exit code of the process. */
        if (WIFEXITED(status)) {
            /* JVM has exited. */
            exitCode = WEXITSTATUS(status);
            res = WRAPPER_PROCESS_DOWN;

            wrapperJVMProcessExited(nowTicks, exitCode);
        } else if (WIFSIGNALED(status)) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS,
                TEXT("JVM received a signal %s (%d)."), getSignalName(WTERMSIG(status)), WTERMSIG(status));
            res = WRAPPER_PROCESS_UP;
        } else if (WIFSTOPPED(status)) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN,
                TEXT("JVM process was stopped.  It will be killed if the ping timeout expires."));
            wrapperData->jvmStopped = TRUE;
            res = WRAPPER_PROCESS_UP;
        } else {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG,
                TEXT("JVM process signaled the Wrapper unexpectedly."));
            res = WRAPPER_PROCESS_UP;
        }
    }

    return res;
}

/**
 * This function does nothing on Unix machines.
 */
void wrapperReportStatus(int useLoggerQueue, int status, int errorCode, int waitHint) {
    return;
}

/**
 * Reads a single block of data from the child pipe.
 *
 * @param blockBuffer Pointer to the buffer where the block will be read.
 * @param blockSize Maximum number of bytes to read.
 * @param readCount Pointer to an int which will hold the number of bytes
 *                  actually read by the call.
 *
 * Returns TRUE if there were any problems, FALSE otherwise.
 */
int wrapperReadChildOutputBlock(char *blockBuffer, int blockSize, int *readCount) {
    if (pipedes[PIPE_READ_END] == -1) {
        /* The child is not up. */
        *readCount = 0;
        return FALSE;
    }

#ifdef FREEBSD
    /* Work around FreeBSD Bug #kern/64313
     *  http://www.freebsd.org/cgi/query-pr.cgi?pr=kern/64313
     *
     * When linked with the pthreads library the O_NONBLOCK flag is being reset
     *  on the pipedes[PIPE_READ_END] handle.  Not sure yet of the exact event that is causing
     *  this, but once it happens reads will start to block even though calls
     *  to fcntl(pipedes[PIPE_READ_END], F_GETFL) say that the O_NONBLOCK flag is set.
     * Calling fcntl(pipedes[PIPE_READ_END], F_SETFL, O_NONBLOCK) again will set the flag back
     *  again and cause it to start working correctly.  This may only need to
     *  be done once, however, because F_GETFL does not return the accurate
     *  state there is no reliable way to check.  Be safe and always set the
     *  flag. */
    if (fcntl(pipedes[PIPE_READ_END], F_SETFL, O_NONBLOCK) < 0) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT(
            "Failed to set JVM output handle to non blocking mode to read child process output: %s (%d)"),
            getLastErrorText(), errno);
        return TRUE;
    }
#endif

    /* Fill read buffer. */
    *readCount = read(pipedes[PIPE_READ_END], blockBuffer, blockSize);
    if (*readCount < 0) {
        /* No more bytes available, return for now.  But make sure that this was not an error. */
        if (errno == EAGAIN) {
            /* Normal, the call would have blocked as there is no data available. */
        } else {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT(
                "Failed to read console output from the JVM: %s (%d)"),
                getLastErrorText(), errno);
            return TRUE;
        }
    } else if (*readCount == 0) {
        /* We reached the EOF.  This means that the other end of the pipe was closed. */
        close(pipedes[PIPE_READ_END]);
        pipedes[PIPE_READ_END] = -1;
    }

    return FALSE;
}

/**
 * Transform a program into a daemon.
 *
 * The idea is to first fork, then make the child a session leader,
 * and then fork again, so that it, (the session group leader), can
 * exit. This means that we, the grandchild, as a non-session group
 * leader, can never regain a controlling terminal.
 */
void daemonize(int argc, TCHAR** argv) {
    pid_t pid;
    int fd;

    /* Set the auto close flag and close the logfile before doing any forking to avoid
     *  duplicate open files. */
    setLogfileAutoClose(TRUE);
    closeLogfile();

    /* first fork */
    if (wrapperData->isDebugging) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Spawning intermediate process..."));
    }
    if ((pid = fork()) < 0) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT("Could not spawn daemon process: %s"),
            getLastErrorText());
        appExit(wrapperData->errorExitCode, argc, argv);
    } else if (pid != 0) {
        /* Intermediate process is now running.  This is the original process, so exit. */

        /* If the main process was not launched in the background, then we want to make
         * the console output look nice by making sure that all output from the
         * intermediate and daemon threads are complete before this thread exits.
         * Sleep for 0.5 seconds. */
        wrapperSleep(500);

        /* Call exit rather than appExit as we are only exiting this process. */
        exit(0);
    }

    /* become session leader */
    if (setsid() == -1) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT("setsid() failed: %s"),
           getLastErrorText());
        appExit(wrapperData->errorExitCode, argc, argv);
    }

    signal(SIGHUP, SIG_IGN); /* don't let future opens allocate controlling terminals */

    /* Redirect stdin, stdout and stderr before closing to prevent the shell which launched
     *  the Wrapper from hanging when it exits. */
    fd = _topen(TEXT("/dev/null"), O_RDWR, 0);
    if (fd != -1) {
        close(STDIN_FILENO);
        dup2(fd, STDIN_FILENO);
        close(STDOUT_FILENO);
        dup2(fd, STDOUT_FILENO);
        close(STDERR_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd != STDIN_FILENO &&
            fd != STDOUT_FILENO &&
            fd != STDERR_FILENO) {
            close(fd);
        }
    }
    /* Console output was disabled above, so make sure the console log output is disabled
     *  so we don't waste any CPU formatting and sending output to '/dev/null'/ */
    toggleLogDestinations(LOG_DESTINATION_CONSOLE, FALSE);

    /* second fork */
    if (wrapperData->isDebugging) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Spawning daemon process..."));
    }
    if ((pid = fork()) < 0) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT("Could not spawn daemon process: %s"),
            getLastErrorText());
        appExit(wrapperData->errorExitCode, argc, argv);
    } else if (pid != 0) {
        /* Daemon process is now running.  This is the intermediate process, so exit. */
        /* Call exit rather than appExit as we are only exiting this process. */
        exit(0);
    }

    /* Restore the auto close flag in the daemonized process. */
    setLogfileAutoClose(wrapperData->logfileCloseTimeout == 0);
}


/**
 * Sets the working directory to that of the current executable
 */
int setWorkingDir(TCHAR *app) {
    TCHAR *szPath;
    TCHAR* pos;

    /* Get the full path and filename of this program */
    if ((szPath = findPathOf(app, TEXT("Wrapper binary"), TRUE)) == NULL) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Unable to get the path for '%s' - %s"),
            app, getLastErrorText());
        return 1;
    }

    /* The wrapperData->isDebugging flag will never be set here, so we can't really use it. */
#ifdef _DEBUG
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Executable Name: %s"), szPath);
#endif

    /* To get the path, strip everything off after the last '\' */
    pos = _tcsrchr(szPath, TEXT('/'));
    if (pos == NULL) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Unable to extract path from: %s"), szPath);
        free(szPath);
        return 1;
    } else {
        /* Clip the path at the position of the last backslash */
        pos[0] = (TCHAR)0;
    }

    /* Set a variable to the location of the binary. */
    setEnv(TEXT("WRAPPER_BIN_DIR"), szPath, ENV_SOURCE_APPLICATION);

    if (wrapperSetWorkingDir(szPath)) {
        free(szPath);
        return 1;
    }
    free(szPath);
    return 0;
}

/*******************************************************************************
 * Main function                                                               *
 *******************************************************************************/
#ifndef CUNIT
#ifdef UNICODE
int main(int argc, char **cargv) {
    size_t req;
    TCHAR **argv;
#else
int main(int argc, char **argv) {
#endif
#if defined(_DEBUG) || defined(UNICODE)
    int i;
#endif
    TCHAR *retLocale;
    int localeSet;
    TCHAR *envLang;
    TCHAR *logFilePath;
    int ret;

#ifdef FREEBSD
    /* In the case of FreeBSD, we need to dynamically load and initialize the iconv library to work with all versions of FreeBSD. */
    if (loadIconvLibrary()) {
        /* Already reported. */
        /* Don't call appExit here as we are not far enough along. */
        return 1;
    }
#endif  
  
    /* Set the default locale here so any startup error messages will have a chance of working.
     *  This should be done before converting cargv to argv, because there might be accentued letters in cargv. */
    envLang = _tgetenv(TEXT("LANG"));
    retLocale = _tsetlocale(LC_ALL, TEXT(""));
    if (!retLocale && envLang) {
        /* On some platforms (i.e. Linux ARM), the locale can't be set if LC_ALL is empty.
         *  In such case, set LC_ALL to the value of LANG and try again. */
        setEnv(TEXT("LC_ALL"), envLang, ENV_SOURCE_APPLICATION);
        retLocale = _tsetlocale(LC_ALL, TEXT(""));
    }
    if (retLocale) {
#if defined(UNICODE)
        free(retLocale);
        if (envLang) {
            free(envLang);
        }
#endif
        localeSet = TRUE;
    } else {
        /* Do not free envLang yet. We will use it below to print a warning. */
        localeSet = FALSE;
    }
    
#ifdef UNICODE
    /* Create UNICODE versions of the argv array for internal use. */
    argv = malloc(argc * sizeof(TCHAR *));
    if (!argv) {
        _tprintf(TEXT("Out of Memory in Main\n"));
        appExit(1, 0, NULL);
        return 1;
    }
    for (i = 0; i < argc; i++) {
        req = mbstowcs(NULL, cargv[i], MBSTOWCS_QUERY_LENGTH);
        if (req == (size_t)-1) {
            _tprintf(TEXT("Encoding problem with arguments in Main\n"));
            free(argv);
            appExit(1, 0, NULL);
            return 1;
        }
        argv[i] = malloc(sizeof(TCHAR) * (req + 1));
        if (!argv[i]) {
            _tprintf(TEXT("Out of Memory in Main\n"));
            while (--i > 0) {
                free(argv[i]);
            }
            free(argv);
            appExit(1, 0, NULL);
            return 1;
        }
        mbstowcs(argv[i], cargv[i], req + 1);
        argv[i][req] = TEXT('\0'); /* Avoid bufferflows caused by badly encoded characters. */
    }
#endif

    if ((argc > 1) && (argv[1][0] == TEXT('-')) && isPromptCallCommand(&argv[1][1])) {
        /* This is a request from the launching script. All log output should be disabled. */
        toggleLogDestinations(LOG_DESTINATION_ALL, FALSE);
    } else {
        for (i = 1; i < argc; i++) {
            /* Disable console output if this should be a daemonized process. */
            if (!strcmpIgnoreCase(argv[i], TEXT("wrapper.daemonize=true"))) {
                toggleLogDestinations(LOG_DESTINATION_CONSOLE, FALSE);
                break;
            }
        }
    }

    if (wrapperInitialize()) {
        appExit(1, argc, argv);
        return 1; /* For compiler. */
    }

#ifdef _DEBUG
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("Wrapper DEBUG build!"));
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("Logging initialized."));
#endif
    /* Get the current process. */
    wrapperData->wrapperPID = getpid();

    if (setWorkingDir(argv[0])) {
        appExit(1, argc, argv);
        return 1; /* For compiler. */
    }
#ifdef _DEBUG
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("Working directory set."));
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("Arguments:"));
    for (i = 0; i < argc; i++) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("  argv[%d]=%s"), i, argv[i]);
    }
#endif
    /* Parse the command and configuration file from the command line. */
    if (!wrapperParseArguments(argc, argv)) {
        appExit(1, argc, argv);
        return 1; /* For compiler. */
    }
    if (!localeSet) {
        log_printf_queue(TRUE, WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("Unable to set the locale to '%s'.  Please make sure $LC_* and $LANG are correct."), (envLang ? envLang : TEXT("<NULL>")));
#if defined(UNICODE)
        if (envLang) {
            free(envLang);
        }
#endif
    }
    wrapperLoadHostName();
    /* At this point, we have a command, confFile, and possibly additional arguments. */
    if (!strcmpIgnoreCase(wrapperData->argCommand, TEXT("?")) || !strcmpIgnoreCase(wrapperData->argCommand, TEXT("-help"))) {
        /* User asked for the usage. */
        setSimpleLogLevels();
        wrapperUsage(argv[0]);
        appExit(0, argc, argv);
        return 0; /* For compiler. */
    } else if (!strcmpIgnoreCase(wrapperData->argCommand, TEXT("v")) || !strcmpIgnoreCase(wrapperData->argCommand, TEXT("-version"))) {
        /* User asked for version. */
        setSimpleLogLevels();
        wrapperVersionBanner();
        appExit(0, argc, argv);
        return 0; /* For compiler. */
    } else if (!strcmpIgnoreCase(wrapperData->argCommand, TEXT("h")) || !strcmpIgnoreCase(wrapperData->argCommand, TEXT("-hostid"))) {
        /* Print out a banner containing the HostId. */
        setSimpleLogLevels();
        wrapperVersionBanner();
        showHostIds(LEVEL_STATUS, TRUE);
        appExit(0, argc, argv);
        return 0; /* For compiler. */
    }

    if (loadConfigurationSettings(TRUE)) {
        if (!strcmpIgnoreCase(wrapperData->argCommand, TEXT("-request_default_log_file"))) {
            /* For this request, we can still return the path to the default log file even if the configuration failed to load. */
        } else {
            /* Unable to load the configuration.  Any errors will have already
             *  been reported. */
            if (wrapperData->argConfFileDefault && !wrapperData->argConfFileFound) {
                /* The config file that was being looked for was default and
                 *  it did not exist.  Show the usage. */
                wrapperUsage(argv[0]);
            } else {
                /* There might have been some queued messages logged on configuration load. Queue the following message to make it appear last. */
                log_printf_queue(TRUE, WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("  The Wrapper will stop."));
            }
            appExit(wrapperData->errorExitCode, argc, argv);
            return 1; /* For compiler. */
        }
    }

    /* Set the default umask of the Wrapper process. */
    umask(wrapperData->umask);
    if (!strcmpIgnoreCase(wrapperData->argCommand, TEXT("-translate"))) {
        setSimpleLogLevels();
        /* Print out the string so the caller sees it as its translated output. */
        _tprintf(TEXT("%s"), argv[2]);
        /* Reset silent mode as some queued messages may be printed. */
        toggleLogDestinations(LOG_DESTINATION_ALL, FALSE);
        appExit(0, argc, argv);
        return 0; /* For compiler. */
    } else if (!strcmpIgnoreCase(wrapperData->argCommand, TEXT("-jvm_bits"))) {
        /* Generate the command used to get the Java version but don't stop on failure. */
        if (!wrapperBuildJavaVersionCommand()) {
            wrapperLaunchJavaVersion(NULL, 0);
        }
        appExit(wrapperData->jvmBits, argc, argv);
        return 0; /* For compiler. */
    } else if (!strcmpIgnoreCase(wrapperData->argCommand, TEXT("-request_delta_binary_bits"))) {
        /* Otherwise return the binary bits */
        appExit(_tcscmp(wrapperBits, TEXT("64")) == 0 ? 64 : 32, argc, argv);
        return 0; /* For compiler. */
    } else if (!strcmpIgnoreCase(wrapperData->argCommand, TEXT("-request_log_file"))) {
        setSimpleLogLevels();
        /* Always try to (re)convert to an absolute path (because non-existent relative paths are left unchanged by the logger). */
        logFilePath = getAbsolutePathOfFile(getLogfilePath(), TEXT(""), LEVEL_NONE, FALSE);
        if (logFilePath) {
            /* Check if the path is writable. */
            if (!checkLogfileDir(FALSE)) {
                /* Expand the path (if some tokens are used for rolling) */
                generateCurrentLogFileName(&logFilePath);
                
                /* Print out the log file path. */
                _tprintf(TEXT("%s"), logFilePath);
                ret = 0;
            } else {
                ret = 1;
            }
            free(logFilePath);
        } else {
            ret = 1;
        }
        /* Reset silent mode as some queued messages may be printed. */
        toggleLogDestinations(LOG_DESTINATION_ALL, FALSE);
        appExit(ret, argc, argv);
        return ret; /* For compiler. */
    } else if (!strcmpIgnoreCase(wrapperData->argCommand, TEXT("-request_default_log_file"))) {
        setSimpleLogLevels();
        /* Always try to (re)convert to an absolute path (because non-existent relative paths are left unchanged by the logger). */
        logFilePath = getAbsolutePathOfFile(getDefaultLogfilePath(), TEXT(""), LEVEL_NONE, FALSE);
        if (logFilePath) {
            /* Check if the path is writable. */
            if (!checkLogfileDir(TRUE)) {
                /* The default log file doesn't contain token for rolling. */
                
                /* Print out the log file path. */
                _tprintf(TEXT("%s"), logFilePath);
                ret = 0;
            } else {
                ret = 1;
            }
            free(logFilePath);
        } else {
            ret = 1;
        }
        /* Reset silent mode as some queued messages may be printed. */
        toggleLogDestinations(LOG_DESTINATION_ALL, FALSE);
        appExit(ret, argc, argv);
        return ret; /* For compiler. */
    } else if (!strcmpIgnoreCase(wrapperData->argCommand, TEXT("c")) || !strcmpIgnoreCase(wrapperData->argCommand, TEXT("-console"))) {
        /* Run as a console application */

        /* fork to a Daemonized process if configured to do so. */
        if (wrapperData->daemonize) {
            daemonize(argc, argv);
            
            /* We are now daemonized, so mark this as being a service. */
            wrapperData->isConsole = FALSE;

            /* When we daemonize the Wrapper, its PID changes. Because of the
             *  WRAPPER_PID environment variable, we need to set it again here
             *  and then reload the configuration in case the PID is referenced
             *  in the configuration. */

            /* Get the current process. */
            wrapperData->wrapperPID = getpid();

            if (wrapperData->isDebugging) {
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Reloading configuration."));
            }
    
            /* If the working dir has been changed then we need to restore it before
             *  the configuration can be reloaded.  This is needed to support relative
             *  references to include files. */
            if (wrapperData->workingDir && wrapperData->originalWorkingDir) {
                if (wrapperSetWorkingDir(wrapperData->originalWorkingDir)) {
                    /* Failed to restore the working dir.  Shutdown the Wrapper */
                    /* There might have been some queued messages logged on configuration load. Queue the following message to make it appear last. */
                    log_printf_queue(TRUE, WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("  The Wrapper will stop."));
                    appExit(wrapperData->errorExitCode, argc, argv);
                    return 1; /* For compiler. */
                }
            }
    
            /* Load the properties. */
            if (loadConfigurationSettings(FALSE)) {
                /* Unable to load the configuration.  Any errors will have already
                 *  been reported. */
                if (wrapperData->argConfFileDefault && !wrapperData->argConfFileFound) {
                    /* The config file that was being looked for was default and
                     *  it did not exist.  Show the usage. */
                    wrapperUsage(argv[0]);
                } else {
                    /* There might have been some queued messages logged on configuration load. Queue the following message to make it appear last. */
                    log_printf_queue(TRUE, WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("  The Wrapper will stop."));
                }
                appExit(wrapperData->errorExitCode, argc, argv);
                return 1; /* For compiler. */
            }
        }

        if (checkPidFile()) {
            /* The pid file exists and we are strict, so exit (cleanUpPIDFilesOnExit has not been turned on yet, so we will exit without cleaning the pid files). */
            appExit(wrapperData->errorExitCode, argc, argv);
            return 1; /* For compiler. */
        }
        
        /* From now on:
         *  - all pid files will be cleaned when the Wrapper exits,
         *  - any existing file will be owerwritten. */
        cleanUpPIDFilesOnExit = TRUE;

        if (wrapperWriteStartupPidFiles()) {
            appExit(wrapperData->errorExitCode, argc, argv);
            return 1; /* For compiler. */
        }

        if (wrapperData->isConsole) {
            appExit(wrapperRunConsole(), argc, argv);
        } else {
            appExit(wrapperRunService(), argc, argv);
        }
        return 0; /* For compiler. */
    } else {
        setSimpleLogLevels();
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN, TEXT("Unrecognized option: -%s"), wrapperData->argCommand);
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN, TEXT(""));
        wrapperUsage(argv[0]);
        appExit(wrapperData->errorExitCode, argc, argv);
        return 1; /* For compiler. */
    }
}
#endif

#endif /* ifndef WIN32 */
