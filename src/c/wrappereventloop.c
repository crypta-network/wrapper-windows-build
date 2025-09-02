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
 * This file contains the main event loop and state engine for
 *  the Java Service Wrapper.
 *
 * Author:
 *   Leif Mortenson <leif@tanukisoftware.com>
 *   Ryan Shaw
 */

#include <stdio.h>
#include <time.h>
#include <sys/stat.h>
#include <string.h>

#ifdef WIN32
#include <io.h>

/* MS Visual Studio 8 went and deprecated the POXIX names for functions.
 *  Fixing them all would be a big headache for UNIX versions. */
#pragma warning(disable : 4996)

#else /* UNIX */
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#endif
#include "wrapper.h"
#include "logger.h"
#ifndef WIN32
 #include "wrapper_ulimit.h"
#endif
#include "wrapper_file.h"
#include "wrapper_jvm_launch.h"
#include "wrapper_encoding.h"
#include "wrapper_i18n.h"

#ifndef MAX
#define MAX(a, b)  (((a) > (b)) ? (a) : (b))
#endif
#ifndef WIN32
static int javaINThreadSet = FALSE;
#endif

static int killConfirmStartTicks = 0;

/**
 * Returns a constant text representation of the specified Wrapper State.
 *
 * @param wState The Wrapper State whose name is being requested.
 *
 * @return The requested Wrapper State.
 */
const TCHAR *wrapperGetWState(int wState) {
    const TCHAR *name;
    switch(wState) {
    case WRAPPER_WSTATE_STARTING:
        name = TEXT("STARTING");
        break;
    case WRAPPER_WSTATE_STARTED:
        name = TEXT("STARTED");
        break;
    case WRAPPER_WSTATE_PAUSING:
        name = TEXT("PAUSING");
        break;
    case WRAPPER_WSTATE_PAUSED:
        name = TEXT("PAUSED");
        break;
    case WRAPPER_WSTATE_RESUMING:
        name = TEXT("RESUMING");
        break;
    case WRAPPER_WSTATE_STOPPING:
        name = TEXT("STOPPING");
        break;
    case WRAPPER_WSTATE_STOPPED:
        name = TEXT("STOPPED");
        break;
    default:
        name = TEXT("UNKNOWN");
        break;
    }
    return name;
}

/**
 * Returns a constant text representation of the specified Java State.
 *
 * @param jState The Java State whose name is being requested.
 *
 * @return The requested Java State.
 */
const TCHAR *wrapperGetJState(int jState) {
    const TCHAR *name;
    switch(jState) {
    case WRAPPER_JSTATE_DOWN_CLEAN:
        name = TEXT("DOWN_CLEAN");
        break;
    case WRAPPER_JSTATE_LAUNCH_DELAY:
        name = TEXT("LAUNCH(DELAY)");
        break;
    case WRAPPER_JSTATE_RESTART:
        name = TEXT("RESTART");
        break;
    case WRAPPER_JSTATE_LAUNCH:
        name = TEXT("LAUNCH");
        break;
    case WRAPPER_JSTATE_LAUNCHING:
        name = TEXT("LAUNCHING");
        break;
    case WRAPPER_JSTATE_LAUNCHED:
        name = TEXT("LAUNCHED");
        break;
    case WRAPPER_JSTATE_STARTING:
        name = TEXT("STARTING");
        break;
    case WRAPPER_JSTATE_STARTED:
        name = TEXT("STARTED");
        break;
    case WRAPPER_JSTATE_STOP:
        name = TEXT("STOP");
        break;
    case WRAPPER_JSTATE_STOPPING:
        name = TEXT("STOPPING");
        break;
    case WRAPPER_JSTATE_STOPPED:
        name = TEXT("STOPPED");
        break;
    case WRAPPER_JSTATE_KILLING:
        name = TEXT("KILLING");
        break;
    case WRAPPER_JSTATE_KILL:
        name = TEXT("KILL");
        break;
    case WRAPPER_JSTATE_DOWN_CHECK:
        name = TEXT("DOWN_CHECK");
        break;
    case WRAPPER_JSTATE_DOWN_FLUSH_STDIN:
        name = TEXT("DOWN_FLUSH_STDIN");
        break;
    case WRAPPER_JSTATE_DOWN_FLUSH:
        name = TEXT("DOWN_FLUSH");
        break;
    case WRAPPER_JSTATE_KILLED:
        name = TEXT("KILLED");
        break;
    default:
        name = TEXT("UNKNOWN");
        break;
    }
    return name;
}

int getStateOutputModeForName(const TCHAR *name) {
    if (strcmpIgnoreCase(name, TEXT("CHANGED")) == 0) {
        /* whenever the jState changes or the suspendTimeouts flags is flipped */
        return STATE_OUTPUT_MODE_CHANGED;
    } else if (strcmpIgnoreCase(name, TEXT("SECONDS")) == 0) {
        /* alias of SECONDS_1. */
        return STATE_OUTPUT_MODE_SECONDS | (1 << 8);
    } else if (_tcsstr(name, TEXT("SECONDS_")) == name) {
        /* parse of name is made simple as this is just for debugging. Store the seconds in a left shifted bit. */
        return STATE_OUTPUT_MODE_SECONDS | (__max(1, __min(255, _ttoi(name + 8))) << 8);
    } else if (_tcsstr(name, TEXT("CHANGED_OR_SECONDS_")) == name) {
        return STATE_OUTPUT_MODE_CHANGED | STATE_OUTPUT_MODE_SECONDS | (__max(1, __min(255, _ttoi(name + 19))) << 8);
    } else {
        /* default */
        return STATE_OUTPUT_MODE_DEFAULT;
    }
}

/**
 * Used to filter state output.
 *
 * @param nowTicks The current tick
 * @param pFirstOutput pointer to a boolean which is TRUE if this is the first output.
 *                    Only used with STATE_OUTPUT_MODE_CHANGED.
 *                    The pointer will be updated by the function.
 * @param pPrevJState pointer to an int which indicates the JVM state of the last output.
 *                    Only used with STATE_OUTPUT_MODE_CHANGED.
 *                    The pointer will be updated by the function.
 * @param pPrevTimeoutSuspended pointer to an int which indicates if the timeouts were suspended in the last output.
 *                    Only used with STATE_OUTPUT_MODE_CHANGED and standard.
 *                    The pointer will be updated by the function.
 * @param mask used to enable filtering certain modes.
 *
 * @return TRUE if the output should be printed, FALSE otherwise.
 */
int isStateOutputEnabled(TICKS nowTicks, int *pFirstOutput, int *pPrevJState, int *pPrevTimeoutSuspended, int mask) {
    if (wrapperData->isStateOutputEnabled) {
        if (wrapperData->stateOutputMode & STATE_OUTPUT_MODE_DEFAULT) {
            return TRUE;
        }
        if (wrapperData->stateOutputMode & STATE_OUTPUT_MODE_SECONDS) {
            if (mask & STATE_OUTPUT_MODE_SECONDS) {
                /* The mask allows to filter using this mode. */
                if (nowTicks % (10 * (wrapperData->stateOutputMode >> 8)) == 0) {
                    return TRUE;
                }
            } else {
                return TRUE;
            }
        }
        if (wrapperData->stateOutputMode & STATE_OUTPUT_MODE_CHANGED) {
            if (mask & STATE_OUTPUT_MODE_CHANGED) {
                /* The mask allows to filter using this mode. Check if the state has changed. */
                if ((*pFirstOutput) || (*pPrevJState != wrapperData->jState)
                ) {
                    /* Currently the mode can't be changed from the command file.
                     *  If we add a command, we need to handle correctly the change of mode. */
                    *pPrevJState = wrapperData->jState;
                    *pFirstOutput = FALSE;
                    return TRUE;
                }
            } else {
                return TRUE;
            }
        }
    } else if (wrapperData->stateOutputMode & STATE_OUTPUT_MODE_CHANGED & mask) {
        /* Reset the flag in case output is enabled later */
        *pFirstOutput = TRUE;
    }
    return FALSE;
}

void writeStateFile(const TCHAR *filename, const TCHAR *state, int newUmask
#ifndef WIN32
    , gid_t newGroup
#endif
    ) {
    FILE *fp = NULL;
    int old_umask;
    int cnt = 0;

    /* If other processes are reading the state file it may be locked for a moment.
     *  Avoid problems by trying a few times before giving up. */
    while (cnt < 10) {
        old_umask = umask(newUmask);
        fp = _tfopen(filename, TEXT("w"));
        umask(old_umask);

        if (fp != NULL) {
#ifndef WIN32
            changePidFileGroup(filename, newGroup);
#endif
            _ftprintf(fp, TEXT("%s\n"), state);
            fclose(fp);

            return;
        }

        /* Sleep for a tenth of a second. */
        wrapperSleep(100);

        cnt++;
    }

    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN, TEXT("Unable to write to the status file: %s"), filename);
}

/**
 * Changes the current Wrapper state.
 *
 * wState - The new Wrapper state.
 */
void wrapperSetWrapperState(int wState) {
    if (wrapperData->isStateOutputEnabled) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS,
            TEXT("      Set Wrapper State %s -> %s"),
            wrapperGetWState(wrapperData->wState),
            wrapperGetWState(wState));
    }

    wrapperData->wState = wState;

    if (wrapperData->statusFilename != NULL) {
        writeStateFile(wrapperData->statusFilename, wrapperGetWState(wrapperData->wState), wrapperData->statusFileUmask
#ifndef WIN32
            , wrapperData->statusFileGroup
#endif
            );
    }
}

/**
 * Updates the current state time out.
 *
 * nowTicks - The current tick count at the time of the call, ignored if
 *            delay is negative.
 * delay - The delay in seconds, added to the nowTicks after which the state
 *         will time out, if negative will never time out.
 */
void wrapperUpdateJavaStateTimeout(TICKS nowTicks, int delay) {
    static int firstOutput = TRUE;
    static int prevJState;
    static int prevTimeoutSuspended;
    TICKS newTicks;
    int ignore;
    int tickAge;

    if (delay >= 0) {
        newTicks = wrapperAddToTicks(nowTicks, delay);
        ignore = FALSE;
        if (wrapperData->jStateTimeoutTicksSet) {
            /* We need to make sure that the new delay is longer than the existing one.
             *  This is complicated slightly because the tick counter can be wrapped. */
            tickAge = wrapperGetTickAgeTicks(wrapperData->jStateTimeoutTicks, newTicks);
            if (tickAge <= 0) {
                ignore = TRUE;
            }
        }

        if (ignore) {
            /* The new value is meaningless. */
            if (isStateOutputEnabled(nowTicks, &firstOutput, &prevJState, &prevTimeoutSuspended, STATE_OUTPUT_MASK_NOSECS)) {
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS,
                    TEXT("      Set Java State %s (%d) Ignored Timeout %08x"),
                    wrapperGetJState(wrapperData->jState),
                    delay,
                    wrapperData->jStateTimeoutTicks);
            }
        } else {
            if (isStateOutputEnabled(nowTicks, &firstOutput, &prevJState, &prevTimeoutSuspended, STATE_OUTPUT_MASK_NOSECS)) {
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS,
                    TEXT("      Set Java State %s (%d) Timeout %08x -> %08x"),
                    wrapperGetJState(wrapperData->jState),
                    delay,
                    nowTicks,
                    newTicks);
            }

            wrapperData->jStateTimeoutTicks = newTicks;
            wrapperData->jStateTimeoutTicksSet = 1;
        }
    } else {
        wrapperData->jStateTimeoutTicks = 0;
        wrapperData->jStateTimeoutTicksSet = 0;
    }
}

/**
 * Changes the current Java state.
 *
 * jState - The new Java state.
 * nowTicks - The current tick count at the time of the call, ignored if
 *            delay is negative.
 * delay - The delay in seconds, added to the nowTicks after which the state
 *         will time out, if negative will never time out.
 */
void wrapperSetJavaState(int jState, TICKS nowTicks, int delay) {
    if (wrapperData->isStateOutputEnabled) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("      Set Java State %s -> %s"),
            wrapperGetJState(wrapperData->jState),
            wrapperGetJState(jState));
    }

    if (wrapperData->jState != jState) {
        /* If the state has changed, then the old timeout will never be used.
         *  Clear it here so any new timeout will be used. */
        wrapperData->jStateTimeoutTicks = 0;
        wrapperData->jStateTimeoutTicksSet = 0;
    }
    wrapperData->jState = jState;
    wrapperUpdateJavaStateTimeout(nowTicks, delay);

    if (wrapperData->javaStatusFilename != NULL) {
        writeStateFile(wrapperData->javaStatusFilename, wrapperGetJState(wrapperData->jState), wrapperData->javaStatusFileUmask
#ifndef WIN32
            , wrapperData->javaStatusFileGroup
#endif
            );
    }
}

/**
 * Prints a single line which helps debugging the Wrapper and JVM states, and timeouts.
 */
void printStateOutput(TICKS nowTicks, int nextSleepMs, int sleepCycle) {
    TCHAR buff1[10];
    static int firstOutput = TRUE;
    static int prevJState;
    static int prevTimeoutSuspended;
    
    if (isStateOutputEnabled(nowTicks, &firstOutput, &prevJState, &prevTimeoutSuspended, STATE_OUTPUT_MASK_ALL)) {
        if (wrapperData->jStateTimeoutTicksSet) {
            _sntprintf(buff1, 10, TEXT("%ds"), wrapperGetTickAgeSeconds(nowTicks, wrapperData->jStateTimeoutTicks));
        } else {
            _tcsncpy(buff1, TEXT("N/A"), 10);
        }
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS,
            TEXT("    Ticks=%08x, WrapperState=%s, JVMState=%s JVMStateTimeoutTicks=%08x (%s), Exit=%s, RestartMode=%d, NextSleep=%dms (%d)"),
            nowTicks,
            wrapperGetWState(wrapperData->wState),
            wrapperGetJState(wrapperData->jState),
            wrapperData->jStateTimeoutTicks,
            buff1,
            (wrapperData->exitRequested ? TEXT("true") : TEXT("false")),
            wrapperData->restartRequested,
            nextSleepMs,
            sleepCycle);
    }
}

int wrapperCheckJstateTimeout(TICKS nowTicks, int suspendable) {
    if (wrapperData->jStateTimeoutTicksSet && (wrapperGetTickAgeTicks(wrapperData->jStateTimeoutTicks, nowTicks) >= 0)) {
        return TRUE;
    }
    return FALSE;
}

void displayLaunchingTimeoutMessage() {
    const TCHAR *mainClass;

    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR,
        TEXT("Startup failed: Timed out waiting for a signal from the JVM."));

    mainClass = getStringProperty(properties, TEXT("wrapper.java.mainclass"), TEXT("Main"));

    if ((_tcsstr(mainClass, TEXT("org.tanukisoftware.wrapper.WrapperSimpleApp")) != NULL)
        || (_tcsstr(mainClass, TEXT("org.tanukisoftware.wrapper.WrapperStartStopApp")) != NULL)) {

        /* The user appears to be using a valid main class, so no advice available. */
    } else {
        if (wrapperData->isAdviserEnabled) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ADVICE, TEXT("") );
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ADVICE,
                TEXT("------------------------------------------------------------------------") );
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ADVICE,
                TEXT("Advice:") );
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ADVICE,
                TEXT("The Wrapper consists of a native component as well as a set of classes\nwhich run within the JVM that it launches.  The Java component of the\nWrapper must be initialized promptly after the JVM is launched or the\nWrapper will timeout, as just happened.  Most likely the main class\nspecified in the Wrapper configuration file is not correctly initializing\nthe Wrapper classes:"));
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ADVICE,
                TEXT("    %s"), mainClass);
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ADVICE,
                TEXT("While it is possible to do so manually, the Wrapper ships with helper\nclasses to make this initialization processes automatic.\nPlease review the integration section of the Wrapper's documentation\nfor the various methods which can be employed to launch an application\nwithin the Wrapper:"));
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ADVICE,
                TEXT("    https://wrapper.tanukisoftware.com/doc/english/integrate.html"));
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ADVICE,
                TEXT("------------------------------------------------------------------------") );
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ADVICE, TEXT("") );
        }
    }
}

/**
 * Handles a timeout for a DebugJVM by showing an appropriate message and
 *  resetting internal timeouts.
 */
void handleDebugJVMTimeout(TICKS nowTicks, const TCHAR *message, const TCHAR *timer) {
    if (!wrapperData->debugJVMTimeoutNotified) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN,
            TEXT("------------------------------------------------------------------------") );
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN, TEXT("%s"), message);
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN,
            TEXT("The JVM was launched with debug options so this may be because the JVM\nis currently suspended by a debugger.  Any future timeouts during this\nJVM invocation will be silently ignored."));
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN,
            TEXT("------------------------------------------------------------------------") );
    }
    wrapperData->debugJVMTimeoutNotified = TRUE;

    /* Make this individual state never timeout then continue. */
    if (wrapperData->isStateOutputEnabled) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS,
            TEXT("      DebugJVM timeout.  Disable current %s timeout."), timer);
    }
    wrapperUpdateJavaStateTimeout(nowTicks, -1);
}

/**
 * Tests for the existence of the anchor file.  If it does not exist then
 *  the Wrapper will begin its shutdown process.
 *
 * nowTicks: The tick counter value this time through the event loop.
 */
void anchorPoll(TICKS nowTicks) {
#if defined(WIN32) && !defined(WIN64)
    struct _stat64i32 fileStat;
#else
    struct stat fileStat;
#endif
    int result;

#ifdef _DEBUG
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_INFO,
        TEXT("Anchor timeout=%d, now=%d"), wrapperData->anchorTimeoutTicks, nowTicks);
#endif

    if (wrapperData->anchorFilename) {
        if (wrapperTickExpired(nowTicks, wrapperData->anchorTimeoutTicks)) {
            if (wrapperData->isLoopOutputEnabled) {
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("    Loop: check anchor file"));
            }

            result = _tstat(wrapperData->anchorFilename, &fileStat);
            if (result == 0) {
                /* Anchor file exists.  Do nothing. */
#ifdef _DEBUG
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_INFO,
                    TEXT("The anchor file %s exists."), wrapperData->anchorFilename);
#endif
            } else {
                /* Anchor file is gone. */
#ifdef _DEBUG
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_INFO,
                    TEXT("The anchor file %s was deleted."), wrapperData->anchorFilename);
#endif

                /* Unless we are already doing so, start the shudown process. */
                if (wrapperData->exitRequested || wrapperData->restartRequested ||
                    (wrapperData->jState == WRAPPER_JSTATE_STOP) ||
                    (wrapperData->jState == WRAPPER_JSTATE_STOPPING) ||
                    (wrapperData->jState == WRAPPER_JSTATE_STOPPED) ||
                    (wrapperData->jState == WRAPPER_JSTATE_KILLING) ||
                    (wrapperData->jState == WRAPPER_JSTATE_KILL) ||
                    (wrapperData->jState == WRAPPER_JSTATE_KILLED) ||
                    (wrapperData->jState == WRAPPER_JSTATE_DOWN_CHECK) ||
                    (wrapperData->jState == WRAPPER_JSTATE_DOWN_FLUSH_STDIN) ||
                    (wrapperData->jState == WRAPPER_JSTATE_DOWN_FLUSH) ||
                    (wrapperData->jState == WRAPPER_JSTATE_DOWN_CLEAN)) {
                    /* Already shutting down, so nothing more to do. */
                } else {
                    /* Always force the shutdown as this was an external event. */
                    wrapperStopProcess(0, TRUE);
                }

                /* To make sure that the JVM will not be restarted for any reason,
                 *  start the Wrapper shutdown process as well. */
                if ((wrapperData->wState == WRAPPER_WSTATE_STOPPING) ||
                    (wrapperData->wState == WRAPPER_WSTATE_STOPPED)) {
                    /* Already stopping. */
                } else {
                    /* Start the shutdown process. */
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("Anchor file deleted.  Shutting down."));
                    
                    wrapperSetWrapperState(WRAPPER_WSTATE_STOPPING);
                }
            }

            wrapperData->anchorTimeoutTicks = wrapperAddToTicks(nowTicks, wrapperData->anchorPollInterval);
        }
    }
}

#ifdef TEST_FORTIFY_SOURCE
struct S {
    struct T {
        char buf[5];
        int x;
    } t;
    char buf[20];
} var;
#endif

/**
 * Tests for the existence of the command file.  If it exists then it will be
 *  opened and any included commands will be processed.  On completion, the
 *  file will be deleted.
 *
 * nowTicks: The tick counter value this time through the event loop.
 */
#define MAX_COMMAND_LENGTH 80
void commandPoll(TICKS nowTicks) {
#if defined(WIN32) && !defined(WIN64)
    struct _stat64i32 fileStat;
#else
    struct stat fileStat;
#endif
    int result;
    FILE *stream;
    int cnt;
    TCHAR buffer[MAX_COMMAND_LENGTH];
    TCHAR *c;
    TCHAR *d;
    TCHAR *command;
    TCHAR *param1;
    TCHAR *param2;
    int exitCode;
    int pauseTime;
    int logLevel;
    int oldLowLogLevel;
    int newLowLogLevel;
    int flag;
    int accessViolation = FALSE;
    int bufferOverflow1 = FALSE;
    size_t i;

#ifdef _DEBUG
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_INFO,
        TEXT("Command timeout=%08x, now=%08x"), wrapperData->commandTimeoutTicks, nowTicks);
#endif

    if (wrapperData->commandFilename) {
        if (wrapperTickExpired(nowTicks, wrapperData->commandTimeoutTicks)) {
            if (wrapperData->isLoopOutputEnabled) {
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("    Loop: check command file"));
            }

            result = _tstat(wrapperData->commandFilename, &fileStat);
            if (result == 0) {
                /* Command file exists. */
#ifdef _DEBUG
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_INFO,
                    TEXT("The command file %s exists."), wrapperData->commandFilename);
#endif
                /* We need to be able to lock and then read the command file.  Other
                 *  applications will be creating this file so we need to handle the
                 *  case where it is locked for a few moments. */
                cnt = 0;
                do {
                    stream = _tfopen(wrapperData->commandFilename, TEXT("r+t"));
                    if (stream == NULL) {
                        /* Sleep for a tenth of a second. */
                        wrapperSleep(100);
                    }

                    cnt++;
                } while ((cnt < 10) && (stream == NULL));

                if (stream == NULL) {
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN,
                        TEXT("Unable to read the command file: %s"), wrapperData->commandFilename);
                } else {
                    /* Read in each of the commands line by line. */
                    do {
                        c = _fgetts(buffer, MAX_COMMAND_LENGTH, stream);
                        if (c != NULL) {
                            /* Always strip both ^M and ^J off the end of the line, this is done rather
                             *  than simply checking for \n so that files will work on all platforms
                             *  even if their line feeds are incorrect. */
                            if ((d = _tcschr(buffer, 13 /* ^M */)) != NULL) {
                                d[0] = TEXT('\0');
                            }
                            if ((d = _tcschr(buffer, 10 /* ^J */)) != NULL) {
                                d[0] = TEXT('\0');
                            }

                            command = buffer;
                            
                            /* Remove any leading space or tabs */
                            while (command[0] == TEXT(' ') || command[0] == TEXT('\t')) {
                                command++;
                            }
                            if (command[0] == TEXT('\0')) {
                                /* Empty line. Ignore it silently. */
                                continue;
                            }
                            /* Remove any tailing space or tabs */
                            i = _tcslen(command) - 1;
                            while (command[i] == TEXT(' ') || command[i] == TEXT('\t')) {
                                i--;
                            }
                            command[i + 1] = TEXT('\0');

                            /** Look for the first space, everything after it will be the parameter(s). */
                            /* Look for parameter 1. */
                            if ((param1 = _tcschr(command, ' ')) != NULL ) {
                                param1[0] = TEXT('\0'); /* Terminate the command. */

                                /* Find the first non-space character. */
                                do {
                                    param1++;
                                } while (param1[0] == TEXT(' '));
                            }
                            if (param1 != NULL) {
                                /* Look for parameter 2. */
                                if ((param2 = _tcschr(param1, ' ')) != NULL ) {
                                    param2[0] = TEXT('\0'); /* Terminate param1. */
    
                                    /* Find the first non-space character. */
                                    do {
                                        param2++;
                                    } while (param2[0] == TEXT(' '));
                                }
                                if (param2 != NULL) {
                                    /* Make sure parameter 2 is terminated. */
                                    if ((d = _tcschr(param2, ' ')) != NULL ) {
                                        d[0] = TEXT('\0'); /* Terminate param2. */
                                    }
                                }
                            } else {
                                param2 = NULL;
                            }

                            /* Process the command. */
                            if (strcmpIgnoreCase(command, TEXT("RESTART")) == 0) {
                                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("Command '%s'. %s"), command, wrapperGetRestartProcessMessage());
                                wrapperRestartProcess();
                            } else if (strcmpIgnoreCase(command, TEXT("STOP")) == 0) {
                                if (param1 == NULL) {
                                    exitCode = 0;
                                } else {
                                    exitCode = _ttoi(param1);
                                }
                                
                                if (exitCode < 0 || exitCode > 255) {
                                    exitCode = wrapperData->errorExitCode;
                                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN,
                                        TEXT("The exit code specified along with the 'STOP' command must be in the range %d to %d.\n  Changing to the default error exit code %d."), 1, 255, exitCode);
                                }
                                
                                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("Command '%s'. Shutting down with exit code %d."), command, exitCode);

                                /* Always force the shutdown as this is an external event. */
                                wrapperStopProcess(exitCode, TRUE);
                                
                                /* To make sure that the JVM will not be restarted for any reason,
                                 *  start the Wrapper shutdown process as well. */
                                if ((wrapperData->wState == WRAPPER_WSTATE_STOPPING) ||
                                    (wrapperData->wState == WRAPPER_WSTATE_STOPPED)) {
                                    /* Already stopping. */
                                } else {
                                    wrapperSetWrapperState(WRAPPER_WSTATE_STOPPING);
                                }
                            } else if (strcmpIgnoreCase(command, TEXT("PAUSE")) == 0) {
                                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("Command '%s'. %s"), command, wrapperGetPauseProcessMessage());
                                wrapperPauseProcess(WRAPPER_ACTION_SOURCE_CODE_COMMANDFILE);
                            } else if (strcmpIgnoreCase(command, TEXT("RESUME")) == 0) {
                                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("Command '%s'. %s"), command, wrapperGetResumeProcessMessage());
                                wrapperResumeProcess(WRAPPER_ACTION_SOURCE_CODE_COMMANDFILE);
                            } else if (strcmpIgnoreCase(command, TEXT("DUMP")) == 0) {
                                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("Command '%s'. Requesting a Thread Dump."), command);
                                wrapperRequestDumpJVMState();
                            } else if (strcmpIgnoreCase(command, TEXT("GC")) == 0) {
                                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("Command '%s'. Requesting a GC."), command);
                                wrapperRequestJVMGC(WRAPPER_ACTION_SOURCE_CODE_COMMANDFILE);
                            } else if ((strcmpIgnoreCase(command, TEXT("CONSOLE_LOGLEVEL")) == 0) ||
                                    (strcmpIgnoreCase(command, TEXT("LOGFILE_LOGLEVEL")) == 0) ||
                                    (strcmpIgnoreCase(command, TEXT("SYSLOG_LOGLEVEL")) == 0)) {
                                if (param1 == NULL) {
                                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN, TEXT("Command '%s' is missing its log level."), command);
                                } else {
                                    logLevel = getLogLevelForName(param1);
                                    if (logLevel == LEVEL_UNKNOWN) {
                                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN, TEXT("Command '%s' specified an unknown log level: '%'"), command, param1);
                                    } else {
                                        oldLowLogLevel = getLowLogLevel();

                                        if (strcmpIgnoreCase(command, TEXT("CONSOLE_LOGLEVEL")) == 0) {
                                            setConsoleLogLevelInt(logLevel);
                                            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("Command '%s'. Set console log level to '%s'."), command, param1);
                                        } else if (strcmpIgnoreCase(command, TEXT("LOGFILE_LOGLEVEL")) == 0) {
                                            setLogfileLevelInt(logLevel);
                                            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("Command '%s'. Set log file log level to '%s'."), command, param1);
                                        } else if (strcmpIgnoreCase(command, TEXT("SYSLOG_LOGLEVEL")) == 0) {
                                            setSyslogLevelInt(logLevel);
                                            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("Command '%s'. Set syslog log level to '%s'."), command, param1);
                                        } else {
                                            /* Shouldn't get here. */
                                            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN, TEXT("Command '%s' lead to an unexpected state."), command);
                                        }

                                        newLowLogLevel = getLowLogLevel();
                                        if (oldLowLogLevel != newLowLogLevel) {
                                            wrapperData->isDebugging = (newLowLogLevel <= LEVEL_DEBUG);

                                            _sntprintf(buffer, MAX_COMMAND_LENGTH, TEXT("%d"), getLowLogLevel());
                                            wrapperProtocolFunction(WRAPPER_MSG_LOW_LOG_LEVEL, buffer);
                                        }
                                    }
                                }
                            } else if ((strcmpIgnoreCase(command, TEXT("LOOP_OUTPUT")) == 0) ||
                                    (strcmpIgnoreCase(command, TEXT("STATE_OUTPUT")) == 0) ||
                                    (strcmpIgnoreCase(command, TEXT("TIMER_OUTPUT")) == 0) ||
                                    (strcmpIgnoreCase(command, TEXT("SLEEP_OUTPUT")) == 0)) {
                                flag = ((param1 != NULL) && (strcmpIgnoreCase(param1, TEXT("TRUE")) == 0));
                                if (strcmpIgnoreCase(command, TEXT("LOOP_OUTPUT")) == 0) {
                                    wrapperData->isLoopOutputEnabled = flag;
                                } else if (strcmpIgnoreCase(command, TEXT("STATE_OUTPUT")) == 0) {
                                    wrapperData->isStateOutputEnabled = flag;
                                } else if (strcmpIgnoreCase(command, TEXT("TIMER_OUTPUT")) == 0) {
                                    wrapperData->isTickOutputEnabled = flag;
                                } else if (strcmpIgnoreCase(command, TEXT("SLEEP_OUTPUT")) == 0) {
                                    wrapperData->isSleepOutputEnabled = flag;
                                }
                                if (flag) {
                                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("Command '%s'. Enable %s."), command, command);
                                } else {
                                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("Command '%s'. Disable %s."), command, command);
                                }
                            } else if (strcmpIgnoreCase(command, TEXT("NAK")) == 0) {
                                if (wrapperData->commandFileTests) {
                                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN, TEXT("Command '%s'.  Sending NAK signal to the JVM..."), command);
                                    wrapperProtocolFunction(MSG_NAK, TEXT("\x03\x03"));
                                } else {
                                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN, TEXT("Command '%s'.  Tests disabled."), command);
                                }
                            } else if ((strcmpIgnoreCase(command, TEXT("CLOSE_SOCKET")) == 0) || (strcmpIgnoreCase(command, TEXT("CLOSE_BACKEND")) == 0)) {
                                if (wrapperData->commandFileTests) {
                                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN, TEXT("Command '%s'.  Closing backend socket to JVM..."), command);
                                    wrapperProtocolClose();
                                } else {
                                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN, TEXT("Command '%s'.  Tests disabled."), command);
                                }
                            } else if (strcmpIgnoreCase(command, TEXT("PAUSE_THREAD")) == 0) {
                                if (wrapperData->commandFileTests) {
                                    if (param2 == NULL) {
                                        pauseTime = -1;
                                    } else {
                                        pauseTime = __max(0, __min(3600, _ttoi(param2)));
                                    }
                                    if (strcmpIgnoreCase(param1, TEXT("MAIN")) == 0) {
                                        wrapperData->pauseThreadMain = pauseTime;
                                    } else if (strcmpIgnoreCase(param1, TEXT("TIMER")) == 0) {
                                        wrapperData->pauseThreadTimer = pauseTime;
                                    } else if (strcmpIgnoreCase(param1, TEXT("JAVAIO")) == 0) {
                                        wrapperData->pauseThreadJavaIO = pauseTime;
                                    } else {
                                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN, TEXT("Command '%s'.  Enqueue request to pause unknown thread."), command);
                                        pauseTime = 0;
                                    }
                                    if (pauseTime > 0) {
                                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN, TEXT("Command '%s'.  Enqueue request to pause %s thread for %d seconds..."), command, param1, pauseTime);
                                    } else if (pauseTime < 0) {
                                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN, TEXT("Command '%s'.  Enqueue request to pause %s thread indefinitely..."), command, param1);
                                    }
                                } else {
                                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN, TEXT("Command '%s'.  Tests disabled."), command);
                                }
                            } else if (strcmpIgnoreCase(command, TEXT("PAUSE_LOGGER")) == 0) {
                                if (wrapperData->commandFileTests) {
                                    if (param1 == NULL) {
                                        pauseTime = -1;
                                    } else {
                                        pauseTime = __max(0, __min(3600, _ttoi(param1)));
                                    }
                                    if (pauseTime > 0) {
                                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN, TEXT("Command '%s'.  Enqueue request to pause logger for %d seconds..."), command, pauseTime);
                                    } else {
                                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN, TEXT("Command '%s'.  Enqueue request to pause logger indefinitely..."), command);
                                    }
                                    setPauseTime(pauseTime);
                                } else {
                                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN, TEXT("Command '%s'.  Tests disabled."), command);
                                }
                            } else if (strcmpIgnoreCase(command, TEXT("ACCESS_VIOLATION")) == 0) {
                                if (wrapperData->commandFileTests) {
                                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Command '%s'.  Intentionally causing an Access Violation in Wrapper..."), command);
                                    /* We can't do the access violation here because we want to make sure the
                                     *  file is deleted first, otherwise it be executed again when the Wrapper is restarted. */
                                    accessViolation = TRUE;
                                } else {
                                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN, TEXT("Command '%s'.  Tests disabled."), command);
                                }
#ifdef TEST_FORTIFY_SOURCE
                            } else if (strcmpIgnoreCase(command, TEXT("BUFFER_OVERFLOW1")) == 0) {
                                if (wrapperData->commandFileTests) {
                                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Command '%s'.  Intentionally causing a Buffer Overflow in Wrapper..."), command);
                                    bufferOverflow1 = TRUE;
                                } else {
                                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN, TEXT("Command '%s'.  Tests disabled."), command);
                                }
#endif
                            } else {
                                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN, TEXT("Command '%s' is unknown, ignoring."), command);
                            }
                        }
                    } while (c != NULL);

                    /* Close the file. */
                    fclose(stream);

                    /* Delete the file. */
                    if (_tremove(wrapperData->commandFilename) == -1) {
                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL,
                            TEXT("Unable to delete the command file, %s: %s"),
                            wrapperData->commandFilename, getLastErrorText());
                    } else {
                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_INFO,
                            TEXT("Command file has been processed and deleted."));
                    }
                    
                    if (accessViolation || bufferOverflow1) {
                        /* Make sure that everything is logged before the crash. */
                        flushLogfile();
                        
                        if (accessViolation) {
                            /* Actually cause the access violation. */
                            c = NULL;
                            c[0] = TEXT('\0');
                            /* Should never get here. */
                        }
#ifdef TEST_FORTIFY_SOURCE
                        if (bufferOverflow1) {
                            /* Actually cause the buffer overflow. */
                            strcpy (&var.t.buf[1], "abcdefg");
                            /* Should never get here. */
                        }
#endif
                    }
                }
            } else {
                /* Command file does not exist. */
#ifdef _DEBUG
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_INFO,
                    TEXT("The command file %s does not exist."), wrapperData->commandFilename);
#endif
            }

            wrapperData->commandTimeoutTicks = wrapperAddToTicks(nowTicks, wrapperData->commandPollInterval);
        }
    }
}

/********************************************************************
 * Wrapper States
 *******************************************************************/
/**
 * WRAPPER_WSTATE_STARTING
 * The Wrapper process is being started.  It will remain in this state
 *  until a JVM and its application has been successfully started.
 *
 * nowTicks: The tick counter value this time through the event loop.
 */
void wStateStarting(TICKS nowTicks) {
    /* While the wrapper is starting up, we need to ping the service  */
    /*  manager to reasure it that we are still alive. */

#ifdef WIN32
    /* Tell the service manager that we are starting */
    wrapperReportStatus(FALSE, WRAPPER_WSTATE_STARTING, 0, wrapperData->ntStartupWaitHint * 1000);
#endif

    /* If we are supposed to pause on startup, we need to jump to that state now, and report that we are started. */
    if (wrapperData->initiallyPaused && wrapperData->pausable) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("Initially Paused."));
        
        wrapperSetWrapperState(WRAPPER_WSTATE_PAUSED);

#ifdef WIN32
        /* Tell the service manager that we started */
        wrapperReportStatus(FALSE, WRAPPER_WSTATE_PAUSED, 0, 0);
#endif
    } else {
        /* If the JVM state is now STARTED, then change the wrapper state */
        /*  to be STARTED as well. */
        if (wrapperData->jState == WRAPPER_JSTATE_STARTED) {
            wrapperSetWrapperState(WRAPPER_WSTATE_STARTED);
    
#ifdef WIN32
            /* Tell the service manager that we started */
            wrapperReportStatus(FALSE, WRAPPER_WSTATE_STARTED, 0, 0);
#endif
        }
    }
}

/**
 * WRAPPER_WSTATE_STARTED
 * The Wrapper process is started.  It will remain in this state until
 *  the Wrapper is ready to start shutting down.  The JVM process may
 *  be restarted one or more times while the Wrapper is in this state.
 *
 * nowTicks: The tick counter value this time through the event loop.
 */
void wStateStarted(TICKS nowTicks) {
    /* Just keep running.  Nothing to do here. */
}

/**
 * WRAPPER_WSTATE_PAUSING
 * The Wrapper process is being paused.  If stopping the JVM is enabled
 *  then it will remain in this state until the JVM has been stopped.
 *  Otherwise it will immediately go to the WRAPPER_WSTATE_PAUSED state.
 *
 * nowTicks: The tick counter value this time through the event loop.
 */
void wStatePausing(TICKS nowTicks) {
    /* While the wrapper is pausing, we need to ping the service  */
    /*  manager to reasure it that we are still alive. */

    /* If we are configured to do so, stop the JVM */
    if (wrapperData->pausableStopJVM) {
        /* If it has not already been set, set the exit request flag. */
        if (wrapperData->jState == WRAPPER_JSTATE_DOWN_CLEAN) {
            /* JVM is now down.  We are now paused. */
            wrapperSetWrapperState(WRAPPER_WSTATE_PAUSED);

#ifdef WIN32
            /* Tell the service manager that we are paused */
            wrapperReportStatus(FALSE, WRAPPER_WSTATE_PAUSED, 0, 0);
#endif
        } else {
#ifdef WIN32
            /* Tell the service manager that we are pausing */
            wrapperReportStatus(FALSE, WRAPPER_WSTATE_PAUSING, 0, wrapperData->ntShutdownWaitHint * 1000);
#endif

            if (wrapperData->exitRequested ||
                (wrapperData->jState == WRAPPER_JSTATE_STOP) ||
                (wrapperData->jState == WRAPPER_JSTATE_STOPPING) ||
                (wrapperData->jState == WRAPPER_JSTATE_STOPPED) ||
                (wrapperData->jState == WRAPPER_JSTATE_KILLING) ||
                (wrapperData->jState == WRAPPER_JSTATE_KILL) ||
                (wrapperData->jState == WRAPPER_JSTATE_KILLED) ||
                (wrapperData->jState == WRAPPER_JSTATE_DOWN_CHECK) ||
                (wrapperData->jState == WRAPPER_JSTATE_DOWN_FLUSH_STDIN) ||
                (wrapperData->jState == WRAPPER_JSTATE_DOWN_FLUSH)) {
                /* In the process of stopping the JVM. */
            } else {
                /* The JVM needs to be stopped, start that process. */
                wrapperData->exitRequested = TRUE;

                /* Make sure the JVM will be restarted. */
                wrapperData->restartRequested = WRAPPER_RESTART_REQUESTED_CONFIGURED;
            }
        }
    } else {
        /* We want to leave the JVM process as is.  We are now paused. */
        wrapperSetWrapperState(WRAPPER_WSTATE_PAUSED);

#ifdef WIN32
        /* Tell the service manager that we are paused */
        wrapperReportStatus(FALSE, WRAPPER_WSTATE_PAUSED, 0, 0);
#endif
    }
}

/**
 * WRAPPER_WSTATE_PAUSED
 * The Wrapper process is paused.  It will remain in this state until
 *  the Wrapper is resumed or is ready to start shutting down.  The
 *  JVM may be stopped or will remain stopped while the Wrapper is in
 *  this state.
 *
 * nowTicks: The tick counter value this time through the event loop.
 */
void wStatePaused(TICKS nowTicks) {
    /* Just keep running.  Nothing to do here. */
}

/**
 * WRAPPER_WSTATE_RESUMING
 * The Wrapper process is being resumed.  We will remain in this state
 *  until the JVM enters the running state.  It may or may not be initially
 *  started.
 *
 * nowTicks: The tick counter value this time through the event loop.
 */
void wStateResuming(TICKS nowTicks) {
    /* While the wrapper is resuming, we need to ping the service  */
    /*  manager to reasure it that we are still alive. */

    /* If the JVM state is now STARTED, then change the wrapper state */
    /*  to be STARTED as well. */
    if (wrapperData->jState == WRAPPER_JSTATE_STARTED) {
        wrapperSetWrapperState(WRAPPER_WSTATE_STARTED);

#ifdef WIN32
        /* Tell the service manager that we started */
        wrapperReportStatus(FALSE, WRAPPER_WSTATE_STARTED, 0, 0);
#endif
    } else {
        /* JVM is down and so it needs to be started. */
#ifdef WIN32
        /* Tell the service manager that we are resuming */
        wrapperReportStatus(FALSE, WRAPPER_WSTATE_RESUMING, 0, wrapperData->ntStartupWaitHint * 1000);
#endif
    }
}

/**
 * WRAPPER_WSTATE_STOPPING
 * The Wrapper process has started its shutdown process.  It will
 *  remain in this state until it is confirmed that the JVM has been
 *  stopped.
 *
 * nowTicks: The tick counter value this time through the event loop.
 */
void wStateStopping(TICKS nowTicks) {
    /* The wrapper is stopping, we need to ping the service manager
     *  to reasure it that we are still alive. */

#ifdef WIN32
    /* Tell the service manager that we are stopping */
    wrapperReportStatus(FALSE, WRAPPER_WSTATE_STOPPING, wrapperData->exitCode, wrapperData->ntShutdownWaitHint * 1000);
#endif

    /* If the JVM state is now DOWN_CLEAN, then change the wrapper state
     *  to be STOPPED as well. */
    if (wrapperData->jState == WRAPPER_JSTATE_DOWN_CLEAN) {
        wrapperSetWrapperState(WRAPPER_WSTATE_STOPPED);

        /* Don't tell the service manager that we stopped here.  That
         *  will be done when the application actually quits. */
    }
}

/**
 * WRAPPER_WSTATE_STOPPED
 * The Wrapper process is now ready to exit.  The event loop will complete
 *  and the Wrapper process will exit.
 *
 * nowTicks: The tick counter value this time through the event loop.
 */
void wStateStopped(TICKS nowTicks) {
    /* The wrapper is ready to stop.  Nothing to be done here.  This */
    /*  state will exit the event loop below. */
}

/********************************************************************
 * JVM States
 *******************************************************************/

/**
 * WRAPPER_JSTATE_DOWN_CLEAN
 * The JVM process currently does not exist and we are clean.  Depending
 *  on the Wrapper state and other factors, we will either stay in this
 *  state or switch to the LAUNCH state causing a JVM to be launched
 *  after a delay set in this function.
 *
 * nowTicks: The tick counter value this time through the event loop.
 */
void jStateDownClean(TICKS nowTicks) {
    TCHAR onExitParamBuffer[16 + 10 + 1];
    const TCHAR *onExitAction;
    int startupDelay;
    int restartMode;

    /* The JVM can be down for one of 4 reasons.  The first is that the
     *  wrapper is just starting.  The second is that the JVM is being
     *  restarted for some reason, the 3rd is that the wrapper is paused,
     *  and the 4th is that the wrapper is trying to shut down. */
    if ((wrapperData->wState == WRAPPER_WSTATE_STARTING) ||
        (wrapperData->wState == WRAPPER_WSTATE_STARTED) ||
        (wrapperData->wState == WRAPPER_WSTATE_RESUMING)) {

        if (wrapperData->restartRequested) {
            /* A JVM needs to be launched. */
            restartMode = wrapperData->restartRequested;
            wrapperData->restartRequested = WRAPPER_RESTART_REQUESTED_NO;
            wrapperData->stopPacketReceived = FALSE;
            wrapperData->stoppedPacketReceived = FALSE;
            wrapperData->restartPacketReceived = FALSE;

            /* Depending on the number of restarts to date, decide how to handle the (re)start. */
            if (wrapperData->jvmRestarts > 0) {
                if (wrapperData->isDebugging) {
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Preparing to restart with mode %d."), restartMode);
                }

                /* This is not the first JVM, so make sure that we still want to launch. */
                if ((wrapperData->wState == WRAPPER_WSTATE_RESUMING) && wrapperData->pausableStopJVM) {
                    /* We are resuming and the JVM was expected to be stopped.  Always launch
                     *  immediately and reset the failed invocation count.
                     * This mode of restarts works even if restarts have been disabled. */
                    wrapperData->failedInvocationCount = 0;
                    wrapperSetJavaState(WRAPPER_JSTATE_LAUNCH_DELAY, nowTicks, 0);

                } else if ((restartMode == WRAPPER_RESTART_REQUESTED_AUTOMATIC) && wrapperData->isAutoRestartDisabled) {
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("Automatic JVM Restarts disabled.  Shutting down."));
                    wrapperSetWrapperState(WRAPPER_WSTATE_STOPPING);

                } else if (wrapperData->isRestartDisabled) {
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("JVM Restarts disabled.  Shutting down."));
                    wrapperSetWrapperState(WRAPPER_WSTATE_STOPPING);

                } else if (wrapperGetTickAgeSeconds(wrapperData->jvmLaunchTicks, nowTicks) >= wrapperData->successfulInvocationTime) {
                    /* The previous JVM invocation was running long enough that its invocation */
                    /*   should be considered a success.  Reset the failedInvocationStart to   */
                    /*   start the count fresh.                                                */
                    wrapperData->failedInvocationCount = 0;

                    /* Set the state to launch after the restart delay. */
                    wrapperSetJavaState(WRAPPER_JSTATE_LAUNCH_DELAY, nowTicks, wrapperData->restartDelay);

                    if (wrapperData->restartDelay > 0) {
                        if (wrapperData->isDebugging) {
                            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG,
                                TEXT("Waiting %d seconds before launching another JVM."), wrapperData->restartDelay);
                        }
                    }
                } else {
                    /* The last JVM invocation died quickly and was considered to have */
                    /*  been a faulty launch.  Increase the failed count.              */
                    wrapperData->failedInvocationCount++;

                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_INFO,
                        TEXT("JVM was running for %d seconds (less than the successful invocation time of %d seconds).\n  Incrementing failed invocation count (currently %d)."),
                        wrapperGetTickAgeSeconds(wrapperData->jvmLaunchTicks, nowTicks), wrapperData->successfulInvocationTime, wrapperData->failedInvocationCount);

                    /* See if we are allowed to try restarting the JVM again. */
                    if (wrapperData->failedInvocationCount < wrapperData->maxFailedInvocations) {
                        /* Try reslaunching the JVM */

                        /* Set the state to launch after the restart delay. */
                        wrapperSetJavaState(WRAPPER_JSTATE_LAUNCH_DELAY, nowTicks, wrapperData->restartDelay);

                        if (wrapperData->restartDelay > 0) {
                            if (wrapperData->isDebugging) {
                                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG,
                                    TEXT("Waiting %d seconds before launching another JVM."), wrapperData->restartDelay);
                            }
                        }
                    } else {
                        /* Unable to launch another JVM. */
                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL,
                                   TEXT("There were %d failed launches in a row, each lasting less than %d seconds.  Giving up."),
                                   wrapperData->failedInvocationCount, wrapperData->successfulInvocationTime);
                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL,
                                   TEXT("  There may be a configuration problem: please check the logs."));
                        wrapperSetWrapperState(WRAPPER_WSTATE_STOPPING);
                    }
                }
            } else {
                /* This will be the first invocation. */
                wrapperData->failedInvocationCount = 0;

                /* Set the state to launch after the startup delay. */
                if (wrapperData->isConsole) {
                    startupDelay = wrapperData->startupDelayConsole;
                } else {
                    startupDelay = wrapperData->startupDelayService;
                }
                wrapperSetJavaState(WRAPPER_JSTATE_LAUNCH_DELAY, nowTicks, startupDelay);

                if (startupDelay > 0) {
                    if (wrapperData->isDebugging) {
                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG,
                            TEXT("Waiting %d seconds before launching the first JVM."), startupDelay);
                    }
                }
            }
        } else {
            /* The JVM is down, but a restart has not yet been requested.
             *   See if the user has registered any events for the exit code. */
            _sntprintf(onExitParamBuffer, 16 + 10 + 1, TEXT("wrapper.on_exit.%d"), wrapperData->exitCode);
            
            onExitAction = getStringProperty(properties, onExitParamBuffer, getStringProperty(properties, TEXT("wrapper.on_exit.default"), TEXT("shutdown")));
            
            if (wrapperData->shutdownActionTriggered && ((strcmpIgnoreCase(onExitAction, TEXT("restart")) == 0) || (strcmpIgnoreCase(onExitAction, TEXT("pause")) == 0))) {
                onExitAction = TEXT("shutdown");
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_INFO,
                    TEXT("Ignoring the action specified with %s.\n  A shutdown configured with %s was already initiated."),
                        isGeneratedProperty(properties, onExitParamBuffer) == FALSE ? onExitParamBuffer : TEXT("wrapper.on_exit.default"),
                        wrapperData->shutdownActionPropertyName);
            }
            
            if (strcmpIgnoreCase(onExitAction, TEXT("restart")) == 0) {
                /* We want to restart the JVM. */
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS,
                    TEXT("on_exit trigger matched.  Restarting the JVM.  (Exit code: %d)"), wrapperData->exitCode);

                wrapperData->restartRequested = WRAPPER_RESTART_REQUESTED_CONFIGURED;

                /* Fall through, the restart will take place on the next loop. */
            } else if (strcmpIgnoreCase(onExitAction, TEXT("pause")) == 0) {
                /* We want to pause the JVM. */
                if (wrapperData->pausable) {
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS,
                        TEXT("on_exit trigger matched.  Pausing the Wrapper.  (Exit code: %d)"), wrapperData->exitCode);
                    wrapperPauseProcess(WRAPPER_ACTION_SOURCE_CODE_ON_EXIT);
                } else {
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS,
                        TEXT("on_exit trigger matched.  Pausing not enabled.  Restarting the JVM.  (Exit code: %d)"), wrapperData->exitCode);
                    wrapperData->restartRequested = WRAPPER_RESTART_REQUESTED_CONFIGURED;
                }
            } else {
                /* We want to stop the Wrapper. */
                
                if (strcmpIgnoreCase(onExitAction, TEXT("shutdown")) != 0) {
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN, TEXT("Encountered an unexpected value for configuration property %s=%s.  Resolving to %s."),
                        onExitParamBuffer, onExitAction, TEXT("SHUTDOWN"));
                }
                
                wrapperSetWrapperState(WRAPPER_WSTATE_STOPPING);
            }
        }
    } else if (wrapperData->wState == WRAPPER_WSTATE_PAUSED) {
        /* The wrapper is paused. */

        if (wrapperData->pausableStopJVM) {
            /* The stop state is expected. */

            /* Make sure we are setup to restart when the Wrapper is resumed later. */
            wrapperData->restartRequested = WRAPPER_RESTART_REQUESTED_CONFIGURED;
        } else {
            /* The JVM should still be running, but it is not.  Try to figure out why. */
            if (wrapperData->restartRequested) {
                /* The JVM must have crashed.  The restart will be honored when the service
                 *  is resumed. Do nothing for now. */
            } else {
                /* No restart was requested.  So the JVM must have requested a stop.
                 *  Normally, this would result in the service stopping from the paused
                 *  state, but it is possible that an exit code is registered. Check them. */
                /* No need to check wrapperData->shutdownActionTriggered here. Even though the PAUSE would be
                 *  originated from some event, it wouldn't be the direct cause of the JVM being down. */
                _sntprintf(onExitParamBuffer, 16 + 10 + 1, TEXT("wrapper.on_exit.%d"), wrapperData->exitCode);
                
                onExitAction = getStringProperty(properties, onExitParamBuffer, getStringProperty(properties, TEXT("wrapper.on_exit.default"), TEXT("shutdown")));
                if (strcmpIgnoreCase(onExitAction, TEXT("restart")) == 0) {
                    /* We want to restart the JVM.   But not now.  Let the user know. */
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS,
                        TEXT("on_exit trigger matched.  Service is paused, will restart the JVM when resumed.  (Exit code: %d)"), wrapperData->exitCode);

                    /* Make sure we are setup to restart when the Wrapper is resumed later. */
                    wrapperData->restartRequested = WRAPPER_RESTART_REQUESTED_CONFIGURED;

                    /* Fall through, the restart will take place once the service is resumed. */
                } else if (strcmpIgnoreCase(onExitAction, TEXT("pause")) == 0) {
                    /* We are paused as expected. */

                    /* Make sure we are setup to restart when the Wrapper is resumed later. */
                    wrapperData->restartRequested = WRAPPER_RESTART_REQUESTED_CONFIGURED;
                } else {
                    /* We want to stop the Wrapper. */
                    
                    if (strcmpIgnoreCase(onExitAction, TEXT("shutdown")) != 0) {
                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN, TEXT("Encountered an unexpected value for configuration property %s=%s.  Resolving to %d."),
                            onExitParamBuffer, onExitAction, TEXT("SHUTDOWN"));
                    }
                    
                    wrapperSetWrapperState(WRAPPER_WSTATE_STOPPING);
                }
            }
        }
    } else {
        /* The wrapper is shutting down or pausing.  Do nothing. */
    }

    /* Reset the last ping time */
    wrapperData->lastPingTicks = nowTicks;
    wrapperData->lastLoggedPingTicks = nowTicks;
}

/**
 * Log messages after a Java query and then progress through lifecycle states.
 *
 * @param nowTicks The current tick.
 * @param queryResult One of the JAVA_PROC_* constants returned by wrapperQueryJava().
 * @param queryDesc The description of the query.
 *
 * @return -1 if the query resulted in a critical failure, or was interrupted,
 *          1 if the query failed but the Wrapper can continue,
 *          0 if the query completed successfully.
 */
static int postProcessJavaQuery(TICKS nowTicks, int queryResult, const TCHAR* queryDesc) {
    switch(queryResult) {
    case JAVA_PROC_LAUNCH_FAILED:
        /* The same will happen with any JVM instance, so stop. */
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Failed to launch the Java command (%s)."), queryDesc);
        return -1;

    case JAVA_PROC_WAIT_FAILED:
        /* The system might currently be too busy.  Count this as a failed invocation and try restarting.
         *  We already logged the details of the error, so just update the state and return. */
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("Timed out waiting for JVM process (%s)."), queryDesc);
        wrapperData->restartRequested = WRAPPER_RESTART_REQUESTED_AUTOMATIC;
        wrapperData->jvmRestarts++;
        wrapperSetJavaState(WRAPPER_JSTATE_DOWN_CLEAN, nowTicks, -1);
        return 1;

    case JAVA_PROC_KILL_FAILED:
        /* We don't want to accumulate unkilled JVMs, so stop. */
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Failed to forcibly terminate the JVM process (%s), unable to continue."), queryDesc);
        if (wrapperData->restartRequested != WRAPPER_RESTART_REQUESTED_NO) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("  The scheduled restart of the JVM has been cancelled."));
            wrapperData->restartRequested = WRAPPER_RESTART_REQUESTED_NO;
            wrapperData->isRestartDisabled = TRUE;
        }
        return -1;

    case JAVA_PROC_INTERRUPTED:
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("JVM (%s) was interrupted."), queryDesc);
        return -1;

    default:
        return 0;
    }
}

/**
 * WRAPPER_JSTATE_LAUNCH_DELAY
 * Waiting to launch a JVM.  When the state timeout has expired, a JVM
 *  will be launched.
 *
 * nowTicks: The tick counter value this time through the event loop.
 */
void jStateLaunchDelay(TICKS nowTicks) {
    const TCHAR *mainClass;
    int ret;
    int isJava9, isJava24;
    int stop = FALSE; /* Used to allow all module related errors to be logged. */
    int isWrapperJarEmbedded;
    int addWrapperToUpgradeModulePath;
    int addWrapperToClassPath;
    TCHAR *ptr;
    int userDefined;
    int checkJavaCommand;

    /* The Waiting state is set from the DOWN_CLEAN state if a JVM had
     *  previously been launched the Wrapper will wait in this state
     *  until the restart delay has expired.  If this was the first
     *  invocation, then the state timeout will be set to the current
     *  time causing the new JVM to be launced immediately. */
    if ((wrapperData->wState == WRAPPER_WSTATE_STARTING) ||
        (wrapperData->wState == WRAPPER_WSTATE_STARTED) ||
        (wrapperData->wState == WRAPPER_WSTATE_RESUMING)) {

        /* Is it time to proceed? */
        if (wrapperCheckJstateTimeout(nowTicks, FALSE)) {
            /* Launch the new JVM */

            if (wrapperData->jvmRestarts > 0) {
                /* See if the logs should be rolled on Wrapper startup. */
                if (getLogfileRollMode() & ROLL_MODE_JVM) {
                    rollLogs(NULL);
                }

                /* Unless this is the first JVM invocation, make it possible to reload the
                 *  Wrapper configuration file. */
                if (wrapperData->restartReloadConf) {
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS,
                        TEXT("Reloading Wrapper configuration..."));

                    /* If the working dir has been changed then we need to restore it before
                     *  the configuration can be reloaded.  This is needed to support relative
                     *  references to include files.
                     * The working directory will then be restored by loadConfigurationSettings() just below. */
                    if (wrapperData->workingDir && wrapperData->originalWorkingDir) {
                        if (wrapperSetWorkingDir(wrapperData->originalWorkingDir)) {
                            /* Failed to restore the working dir.  Shutdown the Wrapper */
                            goto stop;
                        }
                    }

                    if (loadConfigurationSettings(TRUE)) {
                        /* Failed to reload the configuration.  This is bad.
                         *  The JVM is already down.  Shutdown the Wrapper. */
                        goto stop;
                    }
#ifndef WIN32
                    /* Reset the group of the pid files in case the properties were changed.
                     *  All PID files related to the Java process will be recreated so it is not necessary to reset their group. */
                    changePidFileGroup(wrapperData->pidFilename, wrapperData->pidFileGroup);
                    changePidFileGroup(wrapperData->lockFilename, wrapperData->lockFileGroup);
                    changePidFileGroup(wrapperData->statusFilename, wrapperData->statusFileGroup);
                    changePidFileGroup(wrapperData->anchorFilename, wrapperData->anchorFileGroup);
#endif
                    
                    wrapperSetConsoleTitle();
                    
                    /* Dump the reloaded properties */
                    dumpProperties(properties);
                    
                    /* Dump the environment variables */
                    dumpEnvironment();
                    
#ifndef WIN32
                    showResourceslimits();
#endif
                }
            }

            /* Make sure user is not trying to use the old removed SilverEgg package names. */
            mainClass = getStringProperty(properties, TEXT("wrapper.java.mainclass"), TEXT("Main"));
            if (_tcsstr(mainClass, TEXT("com.silveregg.wrapper.WrapperSimpleApp")) != NULL) {
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR,
                    TEXT("The %s class is no longer supported." ), TEXT("com.silveregg.wrapper.WrapperSimpleApp"));
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR,
                    TEXT("Please use the %s class instead." ), TEXT("com.silveregg.wrapper.WrapperSimpleApp"));
                goto stop;
            } else if (_tcsstr(mainClass, TEXT("com.silveregg.wrapper.WrapperStartStopApp")) != NULL) {
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR,
                    TEXT("The %s class is no longer supported." ), TEXT("com.silveregg.wrapper.WrapperStartStopApp"));
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR,
                    TEXT("Please use the %s class instead." ), TEXT("com.silveregg.wrapper.WrapperStartStopApp"));
                goto stop;
            }

            /* Set the launch time to the curent time */
            wrapperData->jvmLaunchTicks = nowTicks;

            /* Generate a unique key to use when communicating with the JVM */
            wrapperBuildKey();

            /* Check the backend server to make sure it has been initialized.
             *  This is needed so we can pass its port as part of the java command. */
            if (!wrapperCheckServerBackend(TRUE)) {
                /* The backend is not up.  An error should have been reported.  But this means we
                 *  are unable to continue. */
                goto stop;
            }

            /* Java will be executed a few times before launching the application:
             *  1) to query the Java version
             *  2) to query additional information (module, mainclass, etc.) using the WrapperBootstrap class
             *  3) to check that the command line is valid (with --dry-run)
             * To keep things simple, this is currently all done in WRAPPER_JSTATE_LAUNCH_DELAY.
             * An alternative would be to create new states (useful for example if we want to associate them with new events),
             * but the JVM output from these queries could then be mixed with other log messages, statistics, etc. */

            /* Generate the command used to get the Java version. */
            if (wrapperBuildJavaVersionCommand()) {
                /* There was either an out of memory error or we failed to get the Java command. No need to continue. */
                goto stop;
            }

            /* Get the Java version. */
            ret = wrapperLaunchJavaVersion(postProcessJavaQuery, nowTicks);
            if (ret == -1) {
                goto stop;
            } else if (ret == 0) {
                if (!wrapperData->javaVersion) {
                    /* We failed to get the Java Version in logParseJavaVersionOutput(). */
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Failed to resolve the version of Java."));
                    /* Failed. Wrapper shutdown. */
                    goto stop;
                }
                
                /* Make sure that the Java version is in the range in which the Wrapper is allowed to run. */
                if (!wrapperConfirmJavaVersion()) {
                    /* Failed. Wrapper shutdown. */
                    goto stop;
                }

                if (wrapperBuildJavaAdditionalArray()) {
                    goto stop;
                }

                isJava9 = isJavaGreaterOrEqual(wrapperData->javaVersion, TEXT("9"));
                isJava24 = isJavaGreaterOrEqual(wrapperData->javaVersion, TEXT("24"));

                /* Main Module */
                /* updateStringValue(&wrapperData->mainModule, getStringProperty(properties, TEXT("wrapper.app.mainmodule"), NULL));
                if (wrapperData->moduleList && !isJava9) {
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Use of %s requires Java version 9 or above."), TEXT("wrapper.app.mainmodule"));
                    stop = TRUE;
                } */

                /* Module list */
                /* Note: Only root modules or those referenced in the command line need to be listed. The JVM builds a
                 *       dependency graph starting from these.
                 *       Additionally, the specification states that "Every module on the upgrade module path or among
                 *       the system modules that exports at least one package, without qualification, is a root.", so
                 *       those (especially org.tanukisoftware.wrapper) don't need to be added to the list. */
                wrapperData->addWrapperToNativeAccessModuleList = FALSE;
                if (wrapperBuildJavaModulelist(&wrapperData->moduleList, isJava24 ? &wrapperData->nativeAccessModuleList : NULL) < 0) {
                    goto stop;
                }
                if (wrapperData->moduleList) {
                    if (!isJava9) {
                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Use of %s requires Java version 9 or above."), TEXT("wrapper.java.module.<n>"));
                        stop = TRUE;
                    }
                } else {
                    /* Try to get the --add-modules option */
                    if (isInJavaAdditionals(TEXT("--add-modules"), &wrapperData->moduleList)) {
                        if (!isJava9) {
                            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Use of %s requires Java version 9 or above."), TEXT("'wrapper.java.additional.<n>=--add-modules ...'"));
                            stop = TRUE;
                        }
                        if (wrapperData->moduleList && ((wrapperData->moduleList)[0] == 0)) {
                            free(wrapperData->moduleList);
                            wrapperData->moduleList = NULL;
                        }
                    }
                }
                if (isJava24) {
                    if (!wrapperData->nativeAccessModuleList) {
                        /* Try to get the --enable-native-access option */
                        if (isInJavaAdditionals(TEXT("--enable-native-access"), &wrapperData->nativeAccessModuleList)) {
                            if (wrapperData->nativeAccessModuleList) {
                                if ((wrapperData->nativeAccessModuleList)[0] == 0) {
                                    free(wrapperData->nativeAccessModuleList);
                                    wrapperData->nativeAccessModuleList = NULL;
                                } else {
                                    ptr = _tcsstr(wrapperData->nativeAccessModuleList, TEXT("org.tanukisoftware.wrapper"));
                                    if (ptr && ((ptr[26] == 0) || (ptr[26] == TEXT(',')))) {
                                        wrapperData->addWrapperToNativeAccessModuleList = FALSE;
                                    } else {
                                        wrapperData->addWrapperToNativeAccessModuleList = TRUE;
                                    }
                                }
                            }
                        }
                    }
                }

                /* wrapper.jar */
                updateStringValue(&wrapperData->wrapperJar, getStringProperty(properties, TEXT("wrapper.jarfile"), NULL));
                isWrapperJarEmbedded = FALSE;
                if (wrapperData->wrapperJar) {
                    if (strcmpIgnoreCase(wrapperData->wrapperJar, TEXT("EMBEDDED")) == 0) {
                        if (!isJava9) {
                            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Value '%s' for the %s property can only be used with Java 9 or above. Currently using version %s."), TEXT("EMBEDDED"), TEXT("wrapper.jarfile"), wrapperData->javaVersion->displayName);
                            stop = TRUE;
                        }
                        isWrapperJarEmbedded = TRUE;
                    } else if (!wrapperFileExists(wrapperData->wrapperJar)) {
                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Value '%s' of the %s property does not refer to an existing jar file."), wrapperData->wrapperJar, TEXT("wrapper.jarfile"));
                        goto stop;
                    }
                }

                addWrapperToUpgradeModulePath = FALSE;
                if (isJava24 || wrapperData->moduleList || wrapperData->mainModule) {
                    /* Use Java Module(s) - Note: At least one root module is required to launch a modularized application (wrapperData->mainModule is not configurable in 3.5.x and should always be NULL here). */
                    if (!wrapperData->wrapperJar) {
                        if (isJava24) {
                            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("The %s property must be set when using Java %s."), TEXT("wrapper.jarfile"), wrapperData->javaVersion->displayName);
                        } else {
                            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Use of Java modules requires the %s property to be set."), TEXT("wrapper.jarfile"));
                        }
                        /* Note: 'stop=TRUE' (to allow other errors to be printed) should work, but is dangerous if the code changes and assumes wrapperData->wrapperJar is set.
                         *       Prefer to stop.  This message is the only one of its type and is also more visible if it is the last one. */
                        goto stop;
                    } else if (!isWrapperJarEmbedded) {
                        /* wrapper.jar must be added to the upgrade module path unless it is included in the runtime image. */
                        addWrapperToUpgradeModulePath = TRUE;
                    }
                } else {
                    /* No module - use ClassPath */
                }

                /* Module Path */
                if (wrapperBuildJavaModulepath(&wrapperData->modulePath, FALSE, FALSE, NULL) < 0) {
                    goto stop;
                }
                if (wrapperData->modulePath) {
                    if (!isJava9) {
                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Use of %s requires Java version 9 or above."), TEXT("wrapper.java.module_path.<n>"));
                        stop = TRUE;
                    }
                } else {
                    /* Try to get the --module-path (or -p) option */
                    if (isInJavaAdditionals(TEXT("--module-path"), &wrapperData->modulePath)) {
                        if (!isJava9) {
                            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Use of %s requires Java version 9 or above."), TEXT("'wrapper.java.additional.<n>=--module-path ...'"));
                            stop = TRUE;
                        }
                    } else if (isInJavaAdditionals(TEXT("-p"), &wrapperData->modulePath)) {
                        if (!isJava9) {
                            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Use of %s requires Java version 9 or above."), TEXT("'wrapper.java.additional.<n>=-p ...'"));
                            stop = TRUE;
                        }
                    }
                    if (wrapperData->modulePath && ((wrapperData->modulePath)[0] == 0)) {
                        free(wrapperData->modulePath);
                        wrapperData->modulePath = NULL;
                    }
                }

                /* Upgrade Module Path */
                userDefined = FALSE;
                if (wrapperBuildJavaModulepath(&wrapperData->upgradeModulePath, addWrapperToUpgradeModulePath, TRUE, &userDefined) < 0) {
                    goto stop;
                }
                if (wrapperData->upgradeModulePath) {
                    /* It is possible that wrapperData->upgradeModulePath was set by us to add wrapper.jar, so only warn if the property was set by the user. */
                    if (!isJava9 && userDefined) {
                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Use of %s requires Java version 9 or above."), TEXT("wrapper.java.upgrade_module_path.<n>"));
                        stop = TRUE;
                    }
                } else {
                    /* Try to get the --upgrade-module-path option */
                    if (isInJavaAdditionals(TEXT("--upgrade-module-path"), &wrapperData->upgradeModulePath)) {
                        if (!isJava9) {
                            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Use of %s requires Java version 9 or above."), TEXT("'wrapper.java.additional.<n>=--upgrade-module-path ...'"));
                            stop = TRUE;
                        }
                        if (wrapperData->upgradeModulePath && ((wrapperData->upgradeModulePath)[0] == 0)) {
                            free(wrapperData->upgradeModulePath);
                            wrapperData->upgradeModulePath = NULL;
                        }
                    }
                }

                if (wrapperData->modulePath && !wrapperData->moduleList && !wrapperData->mainModule) {
                    /* Note: This message does not apply to the upgrade module path where all modules will be automatically added to the root module list. */
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Using %s without specifying at least one module is not valid."), TEXT("wrapper.java.module_path.<n>"));
                    goto stop;
                }
                if (stop) {
                    goto stop;
                }

                /* ClassPath */
                if (!addWrapperToUpgradeModulePath && wrapperData->wrapperJar && !isWrapperJarEmbedded) {
                    addWrapperToClassPath = TRUE;
                } else {
                    addWrapperToClassPath = FALSE;
                }
                if (wrapperBuildJavaClasspath(&wrapperData->classpath, addWrapperToClassPath) < 0) {
                    goto stop;
                }

                /* Update the CLASSPATH in the environment if requested so the Bootstrap and dry-run calls can access it. */ 
                if (wrapperData->environmentClasspath) {
                    if (setEnv(TEXT("CLASSPATH"), wrapperData->classpath, ENV_SOURCE_APPLICATION)) {
                        /* This can happen if the classpath is too long on Windows. */
                        goto stop;
                    }
                }

                /* Prepare the bootstrap command line. */
                if (strcmpIgnoreCase(mainClass, TEXT("org.tanukisoftware.wrapper.WrapperJarApp")) == 0) {
                    wrapperData->jvmAddOpens = isJava9;
                    updateStringValue(&wrapperData->mainJar, getStringProperty(properties, TEXT("wrapper.app.parameter.1"), NULL));
                    if (!wrapperData->mainJar) {
                        goto stop;
                    }

                    wrapperData->jvmBootstrapMode = BOOTSTRAP_ENTRYPOINT_JAR;
                    if (wrapperBuildJavaBootstrapCommand(wrapperData->jvmBootstrapMode, wrapperData->mainJar)) {
                        goto stop;
                    }
                } else {
                    if ((strcmpIgnoreCase(mainClass, TEXT("org.tanukisoftware.wrapper.WrapperSimpleApp")) == 0) ||
                        (strcmpIgnoreCase(mainClass, TEXT("org.tanukisoftware.wrapper.WrapperStartStopApp")) == 0)) {
                        wrapperData->jvmAddOpens = isJava9;
                        updateStringValue(&wrapperData->mainUsrClass, getStringProperty(properties, TEXT("wrapper.app.parameter.1"), NULL));
                    } else {
                        wrapperData->jvmAddOpens = FALSE;
                        updateStringValue(&wrapperData->mainUsrClass, mainClass);
                    }
                    if (!wrapperData->mainUsrClass) {
                        goto stop;
                    }

                    wrapperData->jvmBootstrapMode = BOOTSTRAP_ENTRYPOINT_MAINCLASS;
                    if (wrapperBuildJavaBootstrapCommand(wrapperData->jvmBootstrapMode, wrapperData->mainUsrClass)) {
                        goto stop;
                    }
                }
                ret = wrapperLaunchBootstrap(postProcessJavaQuery, nowTicks);
                if ((ret == -1) || wrapperData->jvmBootstrapFailed) {
                    goto stop;
                } else if (ret == 0) {
                    /* Resolve the encoding to be used for reading the JVM output (this needs to be done before building the Java command line). */
                    if (resolveJvmEncoding(wrapperData->javaVersion->major, wrapperData->jvmVendor)) {
                        /* Failed to get the encoding of the JVM output.
                         *  Stop here because won't be able to display output correctly. */
                        goto stop;
                    }

                    /* The internal application property array is used when building the command line.
                     *  Building the array here is also useful to verify before starting the appplication
                     *  that the properties can be retrieved correctly. */
                    if (wrapperBuildAppPropertyArray()) {
                        goto stop;
                    }

                    checkJavaCommand = isJava9 && getBooleanProperty(properties, TEXT("wrapper.java.command.check"), TRUE);

                    /* Generate the command used to launch the Java process */
                    if (wrapperBuildJavaCommand(checkJavaCommand)) {
                        /* Failed. Wrapper shutdown. */
                        goto stop;
                    }

                    if (wrapperData->useBackendParameters) {
                        /* If application parameters are going to be sent via the backend, we want to
                         *  verify before starting the appplication that they can be retrieved correctly. */
                        if (wrapperBuildAppParameterArray()) {
                            goto stop;
                        }
                    }

                    /* Check the command used to launch the Java process */
                    if (checkJavaCommand) {
                        /* Launch the command with the --dry-run option. */
                        ret = wrapperLaunchDryJavaApp(postProcessJavaQuery, nowTicks);
                        if (ret == -1) {
                            goto stop;
                        } else if (ret == 1) {
                            /* The system might currently be too busy. We cannot check the java command, but continue. */
                            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN, TEXT("Failed to check the Java command."));
                        } else if (wrapperData->jvmDryRunFailed) {
                            /* The Java command line is invalid */
                            goto stop;
                        }
                    }

                    /* Log a few comments that will explain the JVM behavior. */
                    if (wrapperData->isDebugging) {
                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG,
                            TEXT("%s wrapper.startup.timeout=%d, wrapper.startup.delay.console=%d, wrapper.startup.delay.service=%d, wrapper.restart.delay=%d"),
                            TEXT("Startup Timeouts:"),
                            wrapperData->startupTimeout, wrapperData->startupDelayConsole, wrapperData->startupDelayService, wrapperData->restartDelay);
                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG,
                            TEXT("%s wrapper.ping.interval=%d, wrapper.ping.interval.logged=%d, wrapper.ping.timeout=%d, wrapper.ping.alert.threshold=%d"),
                            TEXT("Ping settings:"),
                            wrapperData->pingInterval, wrapperData->pingIntervalLogged, wrapperData->pingTimeout, wrapperData->pingAlertThreshold);
                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG,
                            TEXT("%s wrapper.shutdown.timeout=%d, wrapper.jvm_exit.timeout=%d, wrapper.jvm_cleanup.timeout=%d, wrapper.jvm_terminate.timeout=%d"),
                            TEXT("Shutdown Timeouts:"), 
                            wrapperData->shutdownTimeout, wrapperData->jvmExitTimeout, wrapperData->jvmCleanupTimeout, wrapperData->jvmTerminateTimeout);
                    }

                    if (wrapperData->jvmRestarts > 0) {
                        wrapperSetJavaState(WRAPPER_JSTATE_RESTART, nowTicks, -1);
                    } else {
                        /* Increment the JVM restart Id to keep track of how many JVMs we have launched. */
                        wrapperData->jvmRestarts++;

                        wrapperSetJavaState(WRAPPER_JSTATE_LAUNCH, nowTicks, -1);
                    }
                }
                goto dispose;
            }
        }
    } else {
        /* The wrapper is shutting down, pausing or paused.  Switch to the
         *  down clean state because the JVM was never launched. */
        wrapperSetJavaState(WRAPPER_JSTATE_DOWN_CLEAN, nowTicks, -1);
    }
    return;

  stop:
    wrapperSetWrapperState(WRAPPER_WSTATE_STOPPING);
    wrapperData->exitCode = wrapperData->errorExitCode;

  dispose:
    wrapperFreeJavaAdditionalArray();
}

/**
 * WRAPPER_JSTATE_RESTART
 * The Wrapper is ready to restart a JVM.
 *
 * nowTicks: The tick counter value this time through the event loop.
 */
void jStateRestart(TICKS nowTicks) {

    if ((wrapperData->wState == WRAPPER_WSTATE_STARTING) ||
        (wrapperData->wState == WRAPPER_WSTATE_STARTED) ||
        (wrapperData->wState == WRAPPER_WSTATE_RESUMING)) {
        /* Increment the JVM restart Id to keep track of how many JVMs we have launched. */
        wrapperData->jvmRestarts++;

        wrapperSetJavaState(WRAPPER_JSTATE_LAUNCH, nowTicks, -1);
    } else {
        /* The wrapper is shutting down, pausing or paused.  Switch to the
         *  down clean state because the JVM was never launched. */
        wrapperSetJavaState(WRAPPER_JSTATE_DOWN_CLEAN, nowTicks, -1);
    }
}

/**
 * WRAPPER_JSTATE_LAUNCH
 * The Wrapper is ready to launch a JVM.
 *
 * nowTicks: The tick counter value this time through the event loop.
 */
void jStateLaunch(TICKS nowTicks) {

    if ((wrapperData->wState == WRAPPER_WSTATE_STARTING) ||
        (wrapperData->wState == WRAPPER_WSTATE_STARTED) ||
        (wrapperData->wState == WRAPPER_WSTATE_RESUMING)) {

        if (!wrapperData->runWithoutJVM) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("Launching a JVM..."));
        }

        if (wrapperLaunchJavaApp()) {
            /* We know that there was a problem launching the JVM process.
             *  If we fail at this level, assume it is a critical problem and don't bother trying to restart later.
             *  A message should have already been logged. */
            wrapperSetJavaState(WRAPPER_JSTATE_DOWN_CLEAN, nowTicks, -1);
        } else {
            /* The JVM was launched.  We still do not know whether the
             *  launch will be successful.  Allow <startupTimeout> seconds before giving up.
             *  This can take quite a while if the system is heavily loaded.
             *  (At startup for example) */
            if (wrapperData->startupTimeout > 0) {
                wrapperSetJavaState(WRAPPER_JSTATE_LAUNCHING, nowTicks, wrapperData->startupTimeout);
            } else {
                wrapperSetJavaState(WRAPPER_JSTATE_LAUNCHING, nowTicks, -1);
            }
        }
    } else {
        /* The wrapper is shutting down, pausing or paused.  Switch to the down clean state because the JVM was never launched. */
        wrapperSetJavaState(WRAPPER_JSTATE_DOWN_CLEAN, nowTicks, -1);
    }
}

/**
 * WRAPPER_JSTATE_LAUNCHING
 * The JVM process has been launched, but there has been no confirmation that
 *  the JVM and its application have started.  We remain in this state until
 *  the state times out or the WrapperManager class in the JVM has sent a
 *  message that it is initialized.
 *
 * nowTicks: The tick counter value this time through the event loop.
 */
void jStateLaunching(TICKS nowTicks) {
    /* Make sure that the JVM process is still up and running */
    if (wrapperCheckAndUpdateProcessStatus(nowTicks, FALSE) == WRAPPER_PROCESS_DOWN) {
        /* The java process is gone.  This will be logged and the state will be changed within the eventloop.  Nothing to be done here. */
        return;
    }
    
    /* The process is up and running.
     * We are waiting in this state until we receive a KEY packet
     *  from the JVM attempting to register.
     * Have we waited too long already */
    if (wrapperCheckJstateTimeout(nowTicks, TRUE)) {
        if (wrapperData->debugJVM) {
            handleDebugJVMTimeout(nowTicks, TEXT("Startup: Timed out waiting for a signal from the JVM."), TEXT("startup"));
        } else {
            displayLaunchingTimeoutMessage();

            /* Give up on the JVM and start trying to kill it. */
            wrapperKillProcess(FALSE);

            /* Restart the JVM. */
            wrapperData->restartRequested = WRAPPER_RESTART_REQUESTED_AUTOMATIC;
        }
    }
}

/**
 * WRAPPER_JSTATE_LAUNCHED
 * The WrapperManager class in the JVM has been initialized.  We are now
 *  ready to request that the application in the JVM be started.
 *
 * nowTicks: The tick counter value this time through the event loop.
 */
void jStateLaunched(TICKS nowTicks) {
#ifndef WIN32
    int i = 0;
#endif
    int ret;

    /* The Java side of the wrapper code has responded to a ping.
     *  Tell the Java wrapper to start the Java application. */
    if (wrapperData->isDebugging) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Start Application."));
    }

#ifndef WIN32
    if (wrapperData->javaNewProcessGroup && !wrapperData->disableConsoleInput) {
        /* Create and initialize the 'JavaIN' thread.
         *  This is done here, after the JVM was confirmed launched, to avoid timing issues with wrapperLaunchJvm() where the
         *  pipes are being created and configured.  At this point, the Java application should not be started yet (except for
         *  integration method 3 where it is theoretically possible to use stdin at an early stage, although not recommended).
         *  The WrapperManager doesn't need stdin for initializing, so it's ok to wait until here. */
        if (!javaINThreadSet) {
            if (initializeJavaIN()) {
                /* Error (already logged).  Give up on stdin for this JVM instance, and continue as it is rarely used anyway. */
            } else {
                /* JavaIN thread successfully created, but wait until it's ready to read before sending back the 'start' packet. */
                while (!wrapperData->javaINReady) {
                    wrapperSleep(1);
                    if (++i >= 2000) {
                        /* 2 seconds should be more than enough for the thread to be ready. */
                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN, TEXT("Timed out waiting for %s thread."), TEXT("JavaIN"));
                        break;
                    }
                }

                /* Remember that the thread was launched for the next JVM instances. */
                javaINThreadSet = TRUE;
            }
        }
    }
#endif

    ret = wrapperProtocolFunction(WRAPPER_MSG_START, TEXT("start"));
    if (ret) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT("Unable to send the start command to the JVM."));

        /* Give up on the JVM and start trying to kill it. */
        wrapperKillProcess(FALSE);

        /* Restart the JVM. */
        wrapperData->restartRequested = WRAPPER_RESTART_REQUESTED_AUTOMATIC;
    } else {
        /* Start command send.  Start waiting for the app to signal
         *  that it has started.  Allow <startupTimeout> seconds before
         *  giving up.  A good application will send starting signals back
         *  much sooner than this as a way to extend this time if necessary. */
        if (wrapperData->startupTimeout > 0) {
            wrapperSetJavaState(WRAPPER_JSTATE_STARTING, nowTicks, wrapperData->startupTimeout);
        } else {
            wrapperSetJavaState(WRAPPER_JSTATE_STARTING, nowTicks, -1);
        }
    }
}

/**
 * WRAPPER_JSTATE_STARTING
 * The JVM is up and the application has been asked to start.  We
 *  stay in this state until we receive confirmation that the
 *  application has been started or the state times out.
 *
 * nowTicks: The tick counter value this time through the event loop.
 */
void jStateStarting(TICKS nowTicks) {
    /* Make sure that the JVM process is still up and running */
    if (wrapperCheckAndUpdateProcessStatus(nowTicks, FALSE) == WRAPPER_PROCESS_DOWN) {
        /* The java process is gone.  This will be logged and the state will be changed within the eventloop.  Nothing to be done here. */
        return;
    }
    
    /* Have we waited too long already */
    if (wrapperCheckJstateTimeout(nowTicks, TRUE)) {
        if (wrapperData->debugJVM) {
            handleDebugJVMTimeout(nowTicks, TEXT("Startup: Timed out waiting for a signal from the JVM."), TEXT("startup"));
        } else {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR,
                       TEXT("Startup failed: Timed out waiting for signal from JVM."));

            /* Give up on the JVM and start trying to kill it. */
            wrapperKillProcess(FALSE);

            /* Restart the JVM. */
            wrapperData->restartRequested = WRAPPER_RESTART_REQUESTED_AUTOMATIC;
        }
    } else {
        /* Keep waiting. */
    }
}

/**
 * WRAPPER_JSTATE_STARTED
 * The application in the JVM has confirmed that it is started.  We will
 *  stay in this state, sending pings to the JVM at regular intervals,
 *  until the JVM fails to respond to a ping, or the JVM is ready to be
 *  shutdown.
 * The pings are sent to make sure that the JVM does not die or hang.
 *
 * nowTicks: The tick counter value this time through the event loop.
 */
#define JSTATESTARTED_MESSAGE_MAXLEN (7 + 8 + 1) /* "silent ffffffff\0" */
void jStateStarted(TICKS nowTicks) {
    int ret;
    TCHAR protocolMessage[JSTATESTARTED_MESSAGE_MAXLEN];
    PPendingPing pendingPing;

    /* Make sure that the JVM process is still up and running */
    if (wrapperCheckAndUpdateProcessStatus(nowTicks, FALSE) == WRAPPER_PROCESS_DOWN) {
        /* The java process is gone.  This will be logged and the state will be changed within the eventloop.  Nothing to be done here. */
        return;
    }
    
    /* Look for any PendingPings which are slow but that we have not yet made a note of.
     *  Don't worry about the posibility of finding more than one in a single pass as that should only happen if the Wrapper process was without CPU for a while.  We will quickly catchup on the following cycles. */
    if (wrapperData->firstUnwarnedPendingPing != NULL) {
        if ((wrapperData->pingAlertThreshold > 0) && (wrapperGetTickAgeTicks(wrapperData->firstUnwarnedPendingPing->slowTicks, nowTicks) >= 0)) {
            /* This PendingPing is considered slow. 
             *  Note: The number of times wrapperPingSlow() is called can be limited if WRAPPER_MAX_PENDING_PINGS is reached. But the 'jvm_ping_slow' event
             *        is mainly used to warn when the JVM is slow before we time out, and having a few warnings should be enough for most use cases. */
            wrapperPingSlow();
            
            /* Remove the PendingPing so it won't be warned again.  It still exists in the main list, so it should not be cleaned up here. */
            wrapperData->firstUnwarnedPendingPing = wrapperData->firstUnwarnedPendingPing->nextPendingPing;
        }
    }
    
    if (wrapperData->pingTimedOut && wrapperData->jStateTimeoutTicksSet && (wrapperGetTickAgeTicks(wrapperData->jStateTimeoutTicks, nowTicks) < 0)) {
        /* No longer in a timeout state. Lets reset the flag to allow for further actions if the JVM happens to hang again. */
        wrapperData->pingTimedOut = FALSE;
    }

    if (wrapperCheckJstateTimeout(nowTicks, TRUE)) {
        /* Have we waited too long already.  The jStateTimeoutTicks is reset each time a ping
         *  response is received from the JVM. */
        if (wrapperData->debugJVM) {
            handleDebugJVMTimeout(nowTicks, TEXT("Ping: Timed out waiting for signal from JVM."), TEXT("ping"));
        } else {
            if (wrapperData->pingTimedOut == FALSE) {
                wrapperPingTimeoutResponded();
                /* This is to ensure only one call to wrapperPingTimeoutResponded() will be done even if the state of the JVM remains started after processing the actions. */
                wrapperData->pingTimedOut = TRUE;
            }
        }
    } else if (wrapperGetTickAgeTicks(wrapperAddToTicks(wrapperData->lastPingTicks, wrapperData->pingInterval), nowTicks) >= 0) {
        /* It is time to send another ping to the JVM */
            if (wrapperData->pendingPingQueueOverflow && (!wrapperData->pendingPingQueueOverflowEmptied)) {
                /* There are already too many pending Pings. Keep sending more pings would not help anything and is a risk of
                 *  filling up the pipe if the frequency is too high and the timeout to kill to the JVM is set to a large value. */

                /* To resume pinging, two strategies would work:
                 *  1) start sending pings again once the queue goes back to WRAPPER_MAX_PENDING_PINGS -1.
                 *  2) start sending pings again once the queue is emptied and reset.
                 *  => I find it more clear to not send any ping until we fully catch up, so I prefer 2).
                 *     Notes: - When the queue will be emptied and reset, a new ping will be sent almost immediately because 
                 *              wrapperData->lastPingTicks will be old enough.
                 *            - A ping is always sent before its corresponding pendingPing object is created. Therefore, when 
                 *              the queue of pendingPings is full, a final ping is sent but not added to the pendingPing queue.
                 *              This ensures that there is at least one more ping waiting for reading in the pipe (or socket)
                 *              following last pendingPing object. When pinging resumes, this final ping will be the first to
                 *              be received by the Wrapper, with minimal delay.
                 */
            } else {
                if (wrapperGetTickAgeTicks(wrapperAddToTicks(wrapperData->lastLoggedPingTicks, wrapperData->pingIntervalLogged), nowTicks) >= 0) {
                    if (wrapperData->isLoopOutputEnabled) {
                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("    Loop: Sending a ping packet."));
                    }
                    _sntprintf(protocolMessage, JSTATESTARTED_MESSAGE_MAXLEN, TEXT("ping %08x"), nowTicks);
                    ret = wrapperProtocolFunction(WRAPPER_MSG_PING, protocolMessage);
                    wrapperData->lastLoggedPingTicks = nowTicks;
                } else {
                    if (wrapperData->isLoopOutputEnabled) {
                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("    Loop: Sending a silent ping packet."));
                    }
                    _sntprintf(protocolMessage, JSTATESTARTED_MESSAGE_MAXLEN, TEXT("silent %08x"), nowTicks);
                    ret = wrapperProtocolFunction(WRAPPER_MSG_PING, protocolMessage);
                }
                if (ret) {
                    /* Failed to send the ping. */
                    if (wrapperData->isDebugging) {
                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("JVM Ping Failed."));
                    }
                } else {
                    /* Ping sent successfully. */
                    if (wrapperData->pendingPingCount >= WRAPPER_MAX_PENDING_PINGS) {
                        if (wrapperData->isDebugging) {
                                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Too many Pending Pings.  Disabling some ping checks until the JVM has caught up."));
                        }
                        wrapperData->pendingPingQueueOverflow = TRUE;
                        wrapperData->pendingPingQueueOverflowEmptied = FALSE;
#ifdef DEBUG_PING_QUEUE
                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("    PING QUEUE Set Overflow"));
#endif
                    } else {
                        pendingPing = malloc(sizeof(PendingPing));
                        if (!pendingPing) {
                            outOfMemory(TEXT("JSS"), 1);
                        } else {
                            memset(pendingPing, 0, sizeof(PendingPing));
                            
                            pendingPing->sentTicks = nowTicks;
                            pendingPing->slowTicks = wrapperAddToTicks(nowTicks, wrapperData->pingAlertThreshold);
                            
                            /*  Add it to the PendingPing queue. */
                            if (wrapperData->firstPendingPing == NULL) {
                                /* The queue was empty. */
                                wrapperData->pendingPingCount = 1;
                                wrapperData->firstUnwarnedPendingPing = pendingPing;
                                wrapperData->firstPendingPing = pendingPing;
                                wrapperData->lastPendingPing = pendingPing;
                            } else {
                                /* Add to the end of an existing queue. */
                                wrapperData->pendingPingCount++;
                                if (wrapperData->firstUnwarnedPendingPing == NULL) {
                                    wrapperData->firstUnwarnedPendingPing = pendingPing;
                                }
                                wrapperData->lastPendingPing->nextPendingPing = pendingPing;
                                wrapperData->lastPendingPing = pendingPing;
                            }
#ifdef DEBUG_PING_QUEUE
                            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("+++ PING QUEUE Size: %d"), wrapperData->pendingPingCount);
#endif
                            
                            if ((wrapperData->pendingPingCount > 1) && wrapperData->isDebugging) {
                                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Pending Pings %d"), wrapperData->pendingPingCount);
                            }
                        }
                    }
                }
                
                if (wrapperData->isLoopOutputEnabled) {
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("    Loop: Sent a ping packet."));
                }
                wrapperData->lastPingTicks = nowTicks;
            }
    } else {
        /* Do nothing.  Keep waiting. */
    }
}

/**
 * WRAPPER_JSTATE_STOP
 * The application in the JVM should be asked to stop but is still running.
 *
 * nowTicks: The tick counter value this time through the event loop.
 */
void jStateStop(TICKS nowTicks) {

    /* Make sure that the JVM process is still up and running */
    if (wrapperCheckAndUpdateProcessStatus(nowTicks, FALSE) == WRAPPER_PROCESS_DOWN) {
        /* The java process is gone.  This will be logged and the state will be changed within the eventloop.  Nothing to be done here. */
        return;
    }
    
    /* Ask the JVM to shutdown. */
    wrapperProtocolFunction(WRAPPER_MSG_STOP, NULL);

    /* Allow up to 5 + <shutdownTimeout> seconds for the application to stop itself. */
    if (wrapperData->shutdownTimeout > 0) {
        wrapperSetJavaState(WRAPPER_JSTATE_STOPPING, nowTicks, 5 + wrapperData->shutdownTimeout);
    } else {
        wrapperSetJavaState(WRAPPER_JSTATE_STOPPING, nowTicks, -1);
    }
}

/**
 * WRAPPER_JSTATE_STOPPING
 * The application in the JVM has been asked to stop but we are still
 *  waiting for a signal that it is stopped.
 *
 * nowTicks: The tick counter value this time through the event loop.
 */
void jStateStopping(TICKS nowTicks) {
    /* Make sure that the JVM process is still up and running */
    if (wrapperCheckAndUpdateProcessStatus(nowTicks, FALSE) == WRAPPER_PROCESS_DOWN) {
        /* The java process is gone.  This will be logged and the state will be changed within the eventloop.  Nothing to be done here. */
        return;
    }
    
    /* Have we waited too long already */
    if (wrapperCheckJstateTimeout(nowTicks, TRUE)) {
        if (wrapperData->debugJVM) {
            handleDebugJVMTimeout(nowTicks, TEXT("Shutdown: Timed out waiting for a signal from the JVM."), TEXT("shutdown"));
        } else {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR,
                       TEXT("Shutdown failed: Timed out waiting for signal from JVM."));

            /* Give up on the JVM and start trying to kill it. */
            wrapperData->exitCode = wrapperData->errorExitCode;
            wrapperKillProcess(FALSE);
        }
    } else {
        /* Keep waiting. */
    }
}

/**
 * WRAPPER_JSTATE_STOPPED
 * The application in the JVM has signaled that it has stopped.  We are now
 *  waiting for the JVM process to exit.  A good application will do this on
 *  its own, but if it fails to exit in a timely manner then the JVM will be
 *  killed.
 * Once the JVM process is gone we go back to the DOWN state.
 *
 * nowTicks: The tick counter value this time through the event loop.
 */
void jStateStopped(TICKS nowTicks) {
    if (wrapperCheckAndUpdateProcessStatus(nowTicks, FALSE) == WRAPPER_PROCESS_DOWN) {
        /* The java process is gone.  This will be logged and the state will be changed within the eventloop.  Nothing to be done here. */
        return;
    }
    
    /* Have we waited too long already */
    if (wrapperCheckJstateTimeout(nowTicks, TRUE)) {
        if (wrapperData->debugJVM) {
            handleDebugJVMTimeout(nowTicks, TEXT("Shutdown: Timed out waiting for the JVM to terminate."), TEXT("JVM exit"));
        } else {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR,
                       TEXT("Shutdown failed: Timed out waiting for the JVM to terminate."));

            /* Give up on the JVM and start trying to kill it. */
            wrapperData->exitCode = wrapperData->errorExitCode;
            wrapperKillProcess(FALSE);
        }
    } else {
        /* Keep waiting. */
    }
}

/**
 * WRAPPER_JSTATE_KILLING
 * The Wrapper is about to kill the JVM.  If thread dumps on exit is enabled
 *  then the Wrapper must wait a few moments between telling the JVM to do
 *  a thread dump and actually killing it.  The Wrapper will sit in this state
 *  while it is waiting.
 *
 * nowTicks: The tick counter value this time through the event loop.
 */
void jStateKilling(TICKS nowTicks) {
    /* Make sure that the JVM process is still up and running */
    if (wrapperCheckAndUpdateProcessStatus(nowTicks, FALSE) == WRAPPER_PROCESS_DOWN) {
        /* The java process is gone.  This will be logged and the state will be changed within the eventloop.  Nothing to be done here. */
        return;
    }
    
    /* Have we waited long enough */
    if (wrapperCheckJstateTimeout(nowTicks, FALSE)) {
        /* It is time to actually kill the JVM. */
        wrapperSetJavaState(WRAPPER_JSTATE_KILL, nowTicks, 0);
    } else {
        /* Keep waiting. */
    }
}

/**
 * WRAPPER_JSTATE_KILL
 * The Wrapper is ready to kill the JVM.
 *
 * nowTicks: The tick counter value this time through the event loop.
 */
void jStateKill(TICKS nowTicks) {

    if (wrapperCheckAndUpdateProcessStatus(nowTicks, FALSE) == WRAPPER_PROCESS_DOWN) {
        /* The java process is gone.  This will be logged and the state will be changed within the eventloop.  Nothing to be done here. */
        return;
    }
    
    /* Have we waited long enough */
    if (wrapperCheckJstateTimeout(nowTicks, FALSE)) {
        /* It is time to actually kill the JVM. */
        if (wrapperKillProcessNow()) {
            /* The attempt to forcibly kill the JVM process failed.  There is no way for us to continue from this point as another JVM can not be safely launched. */
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT("Failed to forcibly terminate the JVM process, unable to continue."));
            if (wrapperData->restartRequested != WRAPPER_RESTART_REQUESTED_NO) {
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT("  The scheduled restart of the JVM has been cancelled."));
                wrapperData->restartRequested = WRAPPER_RESTART_REQUESTED_NO;
                wrapperData->isRestartDisabled = TRUE;
            }
        } else {
            /* The request to kill the JVM was successful, but we do not yet know if it is actually gone.
             *  There was a problem on Windows where the OS did not actually follow through with killing the JVM.
             *  The WRAPPER_JSTATE_KILLED state is used to confirm that the JVM is actually gone. */
            if (wrapperData->jvmTerminateTimeout > 0) {
                wrapperSetJavaState(WRAPPER_JSTATE_KILLED, nowTicks, wrapperData->jvmTerminateTimeout);
            } else {
                wrapperSetJavaState(WRAPPER_JSTATE_KILLED, nowTicks, -1);
            }
            killConfirmStartTicks = nowTicks;
        }
    } else {
        /* Keep waiting. */
    }
}

/**
 * WRAPPER_JSTATE_KILLED
 * The Wrapper has asked the OS to forcibly kill the JVM and is now waiting to confirm that is actually gone.
 *
 * nowTicks: The tick counter value this time through the event loop.
 */
void jStateKillConfirm(TICKS nowTicks) {
    if (wrapperCheckAndUpdateProcessStatus(nowTicks, FALSE) == WRAPPER_PROCESS_DOWN) {
        /* The java process is gone.  This will be logged and the state will be changed within the eventloop.  Nothing to be done here. */
        return;
    }
    
    if (wrapperCheckJstateTimeout(nowTicks, FALSE)) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT("Timed out waiting for the OS to forcibly terminate the JVM process, unable to continue."));
        if (wrapperData->restartRequested != WRAPPER_RESTART_REQUESTED_NO) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT("  The scheduled restart of the JVM has been cancelled."));
            wrapperData->restartRequested = WRAPPER_RESTART_REQUESTED_NO;
            wrapperData->isRestartDisabled = TRUE;
        }
        wrapperSetJavaState(WRAPPER_JSTATE_DOWN_CHECK, nowTicks, -1);
        wrapperStopProcess(wrapperData->errorExitCode, TRUE);
    } else {
        /* Keep waiting. */
        if (wrapperGetTickAgeSeconds(killConfirmStartTicks, nowTicks) == 10) {
            /* Print a message every 10 secs. */
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("Waiting for Java process to terminate..."));
            killConfirmStartTicks = nowTicks;

            /* This property is not documented. */
            if (getBooleanProperty(properties, TEXT("wrapper.jvm_terminate.retry"), 
#ifdef WIN32
                TRUE
#else
                FALSE
#endif
                )) {
                /* On some Windows machines with saturated memory, it has been observed that the Java process remained unresponsive to the termination request.
                 *  This is very hard to reproduce but we were wondering if sending new termination requests at regular intervals would help the process to complete faster. */
#ifdef WIN32
                if (TerminateProcess(wrapperData->javaProcess, 0))
#else
                if (kill(wrapperData->javaPID, SIGKILL) == 0)
#endif
                {
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("  Requesting termination again."));
                } else {
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("  Attempt to terminate process failed: %s"), getLastErrorText());
                }
            }
        }
    }
}

/**
 * WRAPPER_JSTATE_DOWN_CHECK
 * The JVM process currently does not exist but we still need to clean up.
 *  Once we have cleaned up, we will switch to the DOWN_FLUSH state.
 *
 * nowTicks: The tick counter value this time through the event loop.
 */
void jStateDownCheck(TICKS nowTicks) {
    wrapperSetJavaState(WRAPPER_JSTATE_DOWN_FLUSH_STDIN, nowTicks, -1);
}

/**
 * WRAPPER_JSTATE_DOWN_FLUSH_STDIN
 * The JVM process and all its children are confirmed terminated. We now need
 *  to empty STDIN in order to cleanly start with a new series of bytes for the
 *  next JVM. This is only needed if a restart is requested.
 *
 * NOTES: - This state never times out, but it starts printing a message at regular
 *          intervals if the flushing takes too long.
 *          TODO: If we want to implement a timeout, we should think of a way to
 *          inform the process piping in that we are no longer accepting new data.
 *        - This state will not be called if we go directly to the down clean state
 *          before the JVM was even launched. This is intended. The stdin is reset
 *          only if a JVM was launched.
 *
 * nowTicks: The tick counter value this time through the event loop.
 */
void jStateDownFlushStdin(TICKS nowTicks) {
#ifndef WIN32
    static int firstCall = TRUE;
    static TICKS ticksWait;
    static TICKS ticksNextMsg;
    char buffer[1024];
    int retR;
    int i;
    int disableStdin = FALSE;
    int wait = FALSE;

    if (!wrapperData->disableConsoleInput &&
        (wrapperData->javaINFlush) &&
        (wrapperData->javaNewProcessGroup) &&
        (wrapperData->restartRequested) &&
        (wrapperData->wState != WRAPPER_WSTATE_STOPPING) &&
        (wrapperData->wState != WRAPPER_WSTATE_STOPPED)) {
        
        if (firstCall) {
            if (wrapperData->isStateOutputEnabled) {
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS,
                    TEXT("      Waiting for stdin to be flushed."));
            }
            
            /* JVM is gone. The read end of pipeind was closed when forking in the parent, and the write-end was set to auto-close on JVM exit.
             *  In case any of the above failed, just make sure that the pipe is closed as this is needed for JavaIN thread to start flushing stdin data. */
            closeStdinPipe();

            /* Allow up to 10 seconds for the javaIN thread to update its status. */
            ticksWait = wrapperAddToTicks(nowTicks, 10);

            /* While waiting for the thread (or flush) to complete, display the first message after 1 second.
             *  In most cases, the operation should complete much faster. */
            ticksNextMsg = wrapperAddToTicks(nowTicks, 1);
        }
        
        if (!wrapperData->javaINFlushed) {
            if (!wrapperData->javaINFlushing && javaINThreadSet) {
                /* Normally we should go into flushing mode pretty quickly as select() should not block longer than one tick
                 *  and write() should fail immediately once Java is down. However, if the JavaIN thread gets blocked on read()
                 *  for some reason, we don't want that to prevent Java from restarting. */
                if (wrapperGetTickAgeTicks(ticksWait, nowTicks) >= 0) {
                    /* The JavaIN thread is blocked and lost.  Disable stdin. */
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT("The thread reading stdin is not responding. Stop reading console input."));
                    disableStdin = TRUE;
                } else {
                    if (wrapperGetTickAgeTicks(ticksNextMsg, nowTicks) >= 0) {
                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("Waiting for I/O to complete in the JavaIn thread..."));

                        /* Avoid printing this message again. */
                        ticksNextMsg = ticksWait + 1;
                    }
                    wait = TRUE;
                }
            } else {
                /* The thread to read Stdin is created once the launched state of the JVM has been comfirmed,
                 *  but it is possible that the process ended before that. If that's the case, flush stdin
                 *  here wihout creating a new thread. */
                if (javaINThreadSet) {
                    /* Flush occurs in javaIN thread. */
                    wait = TRUE;
                } else {
                    /* Flush stdin here. */
                    if (firstCall) {
                        if (fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK) == -1) {
                            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT("Failed to set read mode for stdin.  %s"), getLastErrorText());
                            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT(" Disable stdin."));
                            disableStdin = TRUE;
                        }
                    }

                    if (!disableStdin) {
                        i = 0;
                        do {
                            retR = read(STDIN_FILENO, buffer, sizeof(buffer));
                            if (retR == 0) {
                                /* End of stream.  May happen for example when piping a file to the wrapper. */
                                disableStdin = TRUE;
                            } else if (retR == -1) {
                                /* On a non-blocking call, EAGAIN means read() would have blocked without O_NONBLOCK, and EINTR can't happen.
                                 *  When the buffer is emptied, there may be a slight delay until the pipe is refilled by the system.
                                 *  We consider the flushing is finished after 10 consecutive 'EAGAIN' every ms. */
                                if ((errno == EAGAIN) && (i++ < 10)) {
                                    wrapperSleep(1);
                                    retR = -2;
                                } else {
                                    /* Flush completed. */
                                    if (fcntl(STDIN_FILENO, F_SETFL, O_SYNC) == -1) {
                                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT("Failed to set read mode for stdin.  %s"), getLastErrorText());
                                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT(" Disable stdin."));
                                        disableStdin = TRUE;
                                    }
                                }
                            } else { /* retR > 0 */
                                /* Still data to read, but let the main loop execute. */
                                wait = TRUE;
                            }
                        } while (retR == -2);
                    }
                }
                if (wait) {
                    if (wrapperGetTickAgeTicks(ticksNextMsg, nowTicks) >= 0) {
                        /* Print the first message after 1 sec. */
                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("Flushing stdin before starting a new JVM..."));

                        /* Print the next message after 10 secs. */
                        ticksNextMsg = wrapperAddToTicks(ticksNextMsg, 10);
                    }
                }
            }

            if (disableStdin) {
                wrapperData->disableConsoleInputPermanent = TRUE;
                wrapperData->disableConsoleInput = TRUE;
            } else if (wait) {
                /* Do no go to next state. */
                if (firstCall) {
                    firstCall = FALSE;
                }
                return;
            }
        }
    }
    
    /* Reset for next JVM. */
    firstCall = TRUE;
#endif
    wrapperSetJavaState(WRAPPER_JSTATE_DOWN_FLUSH, nowTicks, -1);
}

/**
 * WRAPPER_JSTATE_DOWN_FLUSH
 * The JVM process currently does not exist but we still need to confirm that
 *  the backend is closed and the ping queue is flushed.
 *  Once we have processed everything, we will switch to the DOWN_CLEAN state.
 *
 * nowTicks: The tick counter value this time through the event loop.
 */
void jStateDownFlush(TICKS nowTicks) {
    PPendingPing pendingPing;
    
    /* Always proceed after a single cycle. */
    /* TODO - Look into ways of reliably detecting when the backend and stdout piles are closed. */
    
    /* Always close the backend here to make sure we are ready for the next JVM.
     * In normal cases, the backend will have already been closed, but if the JVM
     *  crashed or the Wrapper thread was delayed, then it is possible that it is
     *  still open at this point. */
    wrapperProtocolClose();
    
    /* Make sure that the PendingPing pool is empty so they don't cause strange behavior with the next JVM invocation. */
    if (wrapperData->firstPendingPing != NULL) {
        if (wrapperData->isDebugging) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("%d pings were not replied to when the JVM process exited."), wrapperData->pendingPingCount);
        }
        while (wrapperData->firstPendingPing != NULL) {
            pendingPing = wrapperData->firstPendingPing;
            if (pendingPing->nextPendingPing != NULL) {
                /* This was the first PendingPing of several in the queue. */
                wrapperData->pendingPingCount--;
                if (wrapperData->firstUnwarnedPendingPing == wrapperData->firstPendingPing) {
                    wrapperData->firstUnwarnedPendingPing = pendingPing->nextPendingPing;
                }
                wrapperData->firstPendingPing = pendingPing->nextPendingPing;
                pendingPing->nextPendingPing = NULL;
#ifdef DEBUG_PING_QUEUE
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("--- PING QUEUE Size: %d"), wrapperData->pendingPingCount);
#endif
            } else {
                /* This was the only PendingPing in the queue. */
                wrapperData->pendingPingCount = 0;
                wrapperData->firstUnwarnedPendingPing = NULL;
                wrapperData->firstPendingPing = NULL;
                wrapperData->lastPendingPing = NULL;
#ifdef DEBUG_PING_QUEUE
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("--- PING QUEUE Empty.") );
#endif
            }
            
            /* Free up the pendingPing object. */
            if (pendingPing != NULL) {
                free(pendingPing);
                pendingPing = NULL;
            }
        }
    }
    if (wrapperData->pendingPingQueueOverflow) {
        wrapperData->pendingPingQueueOverflow = FALSE;
        wrapperData->pendingPingQueueOverflowEmptied = FALSE;
#ifdef DEBUG_PING_QUEUE
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("--- PING QUEUE Reset Overflow.") );
#endif
    }
    
    /* We are now down and clean. */
    wrapperSetJavaState(WRAPPER_JSTATE_DOWN_CLEAN, nowTicks, -1);
}

/********************************************************************
 * Event Loop / State Engine
 *******************************************************************/

void logTickTimerStats() {
    struct tm when;
    time_t now, overflowTime;

    TICKS sysTicks;
    TICKS ticks;

    time(&now);

    sysTicks = wrapperGetSystemTicks();

    overflowTime = (time_t)(now - (sysTicks / (1000 / WRAPPER_TICK_MS)));
    when = *localtime(&overflowTime);
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS,
        TEXT("    Last system time tick overflow at: %04d/%02d/%02d %02d:%02d:%02d"),
        when.tm_year + 1900, when.tm_mon + 1, when.tm_mday,
        when.tm_hour, when.tm_min, when.tm_sec);

    overflowTime = (time_t)(now + ((0xffffffffUL - sysTicks) / (1000 / WRAPPER_TICK_MS)));
    when = *localtime(&overflowTime);
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS,
        TEXT("    Next system time tick overflow at: %04d/%02d/%02d %02d:%02d:%02d"),
        when.tm_year + 1900, when.tm_mon + 1, when.tm_mday,
        when.tm_hour, when.tm_min, when.tm_sec);

    if (!wrapperData->useSystemTime) {
        ticks = wrapperGetTicks();

        overflowTime = (time_t)(now - (ticks / (1000 / WRAPPER_TICK_MS)));
        when = *localtime(&overflowTime);
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS,
            TEXT("    Last tick overflow at: %04d/%02d/%02d %02d:%02d:%02d"),
            when.tm_year + 1900, when.tm_mon + 1, when.tm_mday,
            when.tm_hour, when.tm_min, when.tm_sec);

        overflowTime = (time_t)(now + ((0xffffffffUL - ticks) / (1000 / WRAPPER_TICK_MS)));
        when = *localtime(&overflowTime);
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS,
            TEXT("    Next tick overflow at: %04d/%02d/%02d %02d:%02d:%02d"),
            when.tm_year + 1900, when.tm_mon + 1, when.tm_mday,
            when.tm_hour, when.tm_min, when.tm_sec);
    }
}

/**
 * The main event loop for the wrapper.  Handles all state changes and events.
 */
void wrapperEventLoop() {
    int readStatus;
    TICKS nowTicks;
    int uptimeSeconds;
    TICKS lastCycleTicks = wrapperGetTicks();
    int nextSleepMs;
    int sleepCycle;
    int skipSleep;
    int prevWState;
    int prevJState;

    /* Initialize the tick timeouts. */
    wrapperData->anchorTimeoutTicks = lastCycleTicks;
    wrapperData->commandTimeoutTicks = lastCycleTicks;
    wrapperData->logfileCloseTimeoutTicks = lastCycleTicks;
    wrapperData->logfileCloseTimeoutTicksSet = FALSE;
    wrapperData->logfileFlushTimeoutTicks = lastCycleTicks;
    wrapperData->logfileFlushTimeoutTicksSet = FALSE;
    
    /* Always auto-flush untils the main loop is reached. This guaranties us all log outputs even if the Wrapper
     *  stops suddenly or get blocked before this point. (had problems when waiting for network interfaces to be up). */
    setLogfileAutoFlush(wrapperData->logfileFlushTimeout == 0);

    if (wrapperData->isDebugging) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Use tick timer mutex=%s"), wrapperData->useTickMutex ? TEXT("TRUE") : TEXT("FALSE"));
    }
    
    if (wrapperData->isTickOutputEnabled) {
        logTickTimerStats();
    }

    if (wrapperData->isLoopOutputEnabled) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("Event loop started."));
    }

    if (wrapperData->isDebugging) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Main loop sleep settings: max: %dms, step size: %dms, step cycles: %d"), wrapperData->mainLoopMaxSleepMs, wrapperData->mainLoopSleepStepMs, wrapperData->mainLoopStepCycles);
    }
    nextSleepMs = 0;
    sleepCycle = 0;
    skipSleep = FALSE;
    do {
        if (wrapperData->isLoopOutputEnabled) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("    Loop: sleep: %dms, cycle count: %d"), skipSleep ? 0 : nextSleepMs, sleepCycle);
        }
        if (skipSleep) {
            skipSleep = FALSE;
        } else {
            if (nextSleepMs > 0) {
                /* Sleep this cycle to prevent high cpu use. */
                wrapperSleep(nextSleepMs);
            }
            
            /* Make any adjustments to the nextSleepMs and sleepCycle. */
            if (nextSleepMs < wrapperData->mainLoopMaxSleepMs) {
                if (sleepCycle + 1 >= wrapperData->mainLoopStepCycles) {
                    nextSleepMs += wrapperData->mainLoopSleepStepMs;
                    if (nextSleepMs > wrapperData->mainLoopMaxSleepMs) {
                        nextSleepMs = wrapperData->mainLoopMaxSleepMs;
                    }
                    sleepCycle = 0;
                } else {
                    sleepCycle++;
                }
            }
        }
        
        prevWState = wrapperData->wState;
        prevJState = wrapperData->jState;
        
        /* Before doing anything else, always maintain the logger to make sure
         *  that any queued messages are logged before doing anything else.
         *  Called a second time after socket and child output to make sure
         *  that all messages appropriate for the state changes have been
         *  logged.  Failure to do so can result in a confusing sequence of
         *  output. */
        if (wrapperData->isLoopOutputEnabled) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("    Loop: maintain logger"));
        }
        maintainLogger();
        
        if (wrapperData->pauseThreadMain) {
            wrapperPauseThread(wrapperData->pauseThreadMain, TEXT("main"));
            wrapperData->pauseThreadMain = 0;
        }
        
        /* After we maintain the logger, see if there were any signals trapped. */
#ifdef WIN32
        wrapperMaintainControlCodes();
#else
        wrapperMaintainSignals();
#endif

#ifdef WIN32
        /* Check to make sure the Wrapper or Java console windows are hidden.
         *  This is done here to make sure they go away even in cases where they can't be hidden right away.
         * Users have also reported that the console can be redisplayed when a user logs back in or switches users. */
        wrapperCheckConsoleWindows();
#endif

        if (wrapperData->useJavaIOThread) {
            /* Child output is being handled by a dedicated IO thread. */
        } else {
            /* Check the stout pipe of the child process. */
            if (wrapperData->isLoopOutputEnabled) {
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("    Loop: process JVM output"));
            }
            /* Request that the processing of child output not take more than 250ms. */
            if (wrapperReadChildOutput(250)) {
                if (wrapperData->isDebugging) {
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG,
                        TEXT("Pause reading child process output to share cycles."));
                }
                /* There was child output logged in this cycle, so there is a good chance that there will be more output soon.
                 *  To improve performance we never want to sleep in the next cycle of this loop. */
                nextSleepMs = 0;
                sleepCycle = 0;
            } else {
                /* There was not any child output.  There is no way of knowing when output will come, but allow sleeping in the next cycle to prevent thrashing. */
            }
        }

        /* Check for incoming data packets. */
        if (wrapperData->isLoopOutputEnabled) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("    Loop: process socket"));
        }

        /* Process the socket. */
        readStatus = wrapperProtocolRead();

        /* See comment for first call above. */
        if (wrapperData->isLoopOutputEnabled) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("    Loop: maintain logger(2)"));
        }
        maintainLogger();

        switch (readStatus) {
        case WRAPPER_PROTOCOLE_READ_MORE_DATA:
            /* There was more data waiting to be read, but we broke out. */
            if (wrapperData->isDebugging) {
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG,
                    TEXT("Pause reading socket data to share cycles."));
            }
            nextSleepMs = 0;
            sleepCycle = 0;
            break;

        case WRAPPER_PROTOCOLE_READ_SOCKET_EOF:
            /* No more data to read for this JVM instance. */
            wrapperProtocolClose();
            break;

        case WRAPPER_PROTOCOLE_READ_FAILED:
            /* Backend cannot be read, this is a permanent failure. */
            if (jvmStateExpectsBackendData()) {
                if (wrapperQuickCheckJavaProcessStatus() != WRAPPER_PROCESS_DOWN) {
                    log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_ERROR, TEXT("Backend connection closed unexpectedly.  %s"), TEXT("Waiting for the process to complete."));
                }
                wrapperProtocolClose(); /* log here for clarity in message order (client connection closed unexpectedly -> so we close the pipe or socket on the server side) */

                /* The backend can no longer be used, so we won't receive the STOPPED message. But we still want to give the JVM a chance to exit on its own
                 *  (if the java process is still alive, it should detect that the backend connection was interrupted and initiate its shutdown hooks).
                 *  The exit timeout should still apply. */
                wrapperData->restartRequested = WRAPPER_RESTART_REQUESTED_AUTOMATIC;
                if (wrapperData->jvmExitTimeout > 0) {
                    wrapperSetJavaState(WRAPPER_JSTATE_STOPPED, wrapperGetTicks(), 5 + wrapperData->jvmExitTimeout);
                } else {
                    wrapperSetJavaState(WRAPPER_JSTATE_STOPPED, 0, -1);
                }
            } else {
                wrapperProtocolClose();
            }
            break;

        case WRAPPER_PROTOCOLE_OPEN_FAILED:
            /* Server was not started correctly, this is a permanent failure. */
            if (jvmStateExpectsBackendData()) {
                if (wrapperQuickCheckJavaProcessStatus() != WRAPPER_PROCESS_DOWN) {
                    log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_ERROR, TEXT("Backend server not started.  %s"), TEXT("Waiting for the process to complete."));
                }

                /* The backend can no longer be used, so we won't receive the STOPPED message. But we still want to give the JVM a chance to exit on its own
                 *  (if the java process is still alive, it should detect that the backend connection was interrupted and initiate its shutdown hooks).
                 *  The exit timeout should still apply. */
                wrapperData->restartRequested = WRAPPER_RESTART_REQUESTED_AUTOMATIC;
                if (wrapperData->jvmExitTimeout > 0) {
                    wrapperSetJavaState(WRAPPER_JSTATE_STOPPED, wrapperGetTicks(), 5 + wrapperData->jvmExitTimeout);
                } else {
                    wrapperSetJavaState(WRAPPER_JSTATE_STOPPED, 0, -1);
                }
            }
            break;

        case WRAPPER_PROTOCOLE_READ_COMPLETE:
        default:
            /* This read completed successfully. */
            break;
        }

        /* Get the current time for use in this cycle. */
        nowTicks = wrapperGetTicks();

        /* Tell the logging code what to use for the uptime. */
        if (!wrapperData->uptimeFlipped) {
            wrapperSetUptime(nowTicks, &uptimeSeconds);
        }

        /* Test the activity of the logfile. */
        if (getLogfileActivity() != 0) {
            /* There was log output since the last pass. */
            
            /* Set the close timeout if enabled.  This is based on inactivity, so we always want to extend it from the current time when there was output. */
            if (wrapperData->logfileCloseTimeout > 0) {
                wrapperData->logfileCloseTimeoutTicks = wrapperAddToTicks(nowTicks, wrapperData->logfileCloseTimeout);
                wrapperData->logfileCloseTimeoutTicksSet = TRUE;
            }
            
            /* Set the flush timeout if enabled, and it is not already set. */
            if (wrapperData->logfileFlushTimeout > 0) {
                if (!wrapperData->logfileFlushTimeoutTicksSet) {
                    wrapperData->logfileFlushTimeoutTicks = wrapperAddToTicks(nowTicks, wrapperData->logfileFlushTimeout);
                    wrapperData->logfileFlushTimeoutTicksSet = TRUE;
                }
            }
        } else if (wrapperData->logfileCloseTimeoutTicksSet && (wrapperTickExpired(nowTicks, wrapperData->logfileCloseTimeoutTicks))) {
            /* If the inactivity timeout has expired then we want to close the logfile, otherwise simply flush it. */
            closeLogfile();
            
            /* Reset the timeout ticks so we don't start another timeout until something has been logged. */
            wrapperData->logfileCloseTimeoutTicksSet = FALSE;
            /* If we close the file, it is automatically flushed. */
            wrapperData->logfileFlushTimeoutTicksSet = FALSE;
        }
        
        /* Is it is time to flush the logfile? */
        if (wrapperData->logfileFlushTimeoutTicksSet && (wrapperTickExpired(nowTicks, wrapperData->logfileFlushTimeoutTicks))) {
            /* Time to flush the output. */
            flushLogfile();
            /* Reset the timeout until more output is logged. */
            wrapperData->logfileFlushTimeoutTicksSet = FALSE;
        }

        /* Has the process been getting CPU? This check will only detect a lag
         * if the useSystemTime flag is set. */
        if (wrapperGetTickAgeSeconds(lastCycleTicks, nowTicks) > wrapperData->cpuTimeout) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_INFO,
                TEXT("Wrapper Process has not received any CPU time for %d seconds.  Extending timeouts."),
                wrapperGetTickAgeSeconds(lastCycleTicks, nowTicks));

            if (wrapperData->jStateTimeoutTicksSet) {
                wrapperData->jStateTimeoutTicks =
                    wrapperAddToTicks(wrapperData->jStateTimeoutTicks, wrapperGetTickAgeSeconds(lastCycleTicks, nowTicks));
            }
        }
        lastCycleTicks = nowTicks;

        printStateOutput(nowTicks, nextSleepMs, sleepCycle);

        /* If we are configured to do so, confirm that the anchor file still exists. */
        anchorPoll(nowTicks);

        /* If we are configured to do so, look for a command file and perform any
         *  requested operations. */
        commandPoll(nowTicks);

        if (wrapperData->exitRequested) {
            /* A new request for the JVM to be stopped has been made. */

            if (wrapperData->isLoopOutputEnabled) {
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("    Loop: exit requested"));
            }
            /* Acknowledge that we have seen the exit request so we don't get here again. */
            wrapperData->exitRequested = FALSE;

            if (wrapperData->jState == WRAPPER_JSTATE_DOWN_CLEAN) {
                /* A JVM is not currently running. Nothing to do.*/
            } else if ((wrapperData->jState == WRAPPER_JSTATE_LAUNCH_DELAY) ||
                (wrapperData->jState == WRAPPER_JSTATE_RESTART) ||
                (wrapperData->jState == WRAPPER_JSTATE_LAUNCH)) {
                /* A JVM is not yet running go back to the DOWN_CLEAN state. */
                wrapperSetJavaState(WRAPPER_JSTATE_DOWN_CLEAN, nowTicks, -1);
            } else if ((wrapperData->jState == WRAPPER_JSTATE_STOP) ||
                (wrapperData->jState == WRAPPER_JSTATE_STOPPING) ||
                (wrapperData->jState == WRAPPER_JSTATE_STOPPED) ||
                (wrapperData->jState == WRAPPER_JSTATE_KILLING) ||
                (wrapperData->jState == WRAPPER_JSTATE_KILL) ||
                (wrapperData->jState == WRAPPER_JSTATE_KILLED) ||
                (wrapperData->jState == WRAPPER_JSTATE_DOWN_CHECK) ||
                (wrapperData->jState == WRAPPER_JSTATE_DOWN_FLUSH_STDIN) ||
                (wrapperData->jState == WRAPPER_JSTATE_DOWN_FLUSH)) {
                /* The JVM is already being stopped, so nothing else needs to be done. */
            } else {
                /* The JVM should be running or is in the process of launching, so it needs to be stopped. */
                if (wrapperCheckAndUpdateProcessStatus(nowTicks, FALSE) == WRAPPER_PROCESS_DOWN) {
                    /* The process is gone.  (Handled and logged)
                     *  Note: wrapperCheckAndUpdateProcessStatus() may have changed the state to WRAPPER_JSTATE_DOWN_CHECK. */
                    
                    if (wrapperData->restartPacketReceived) {
                        /* The restart packet was received.  If we are here then it was delayed,
                         *  but it means that we do want to restart. */
                    } else {
                        /* We never want to restart here. */
                        wrapperData->restartRequested = WRAPPER_RESTART_REQUESTED_NO;
                        if (wrapperData->isDebugging) {
                            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Reset the restart flag."));
                        }
                    }
                } else {
                    /* JVM is still up.  Try asking it to shutdown nicely. */
                    if (wrapperData->isDebugging) {
                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG,
                            TEXT("Sending stop signal to JVM"));
                    }

                    wrapperSetJavaState(WRAPPER_JSTATE_STOP, nowTicks, -1);
                }
            }
        }

        /* Do something depending on the wrapper state */
        if (wrapperData->isLoopOutputEnabled) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("    Loop: handle wrapper state: %s"),
                wrapperGetWState(wrapperData->wState));
        }
        switch(wrapperData->wState) {
        case WRAPPER_WSTATE_STARTING:
            wStateStarting(nowTicks);
            break;

        case WRAPPER_WSTATE_STARTED:
            wStateStarted(nowTicks);
            break;

        case WRAPPER_WSTATE_PAUSING:
            wStatePausing(nowTicks);
            break;

        case WRAPPER_WSTATE_PAUSED:
            wStatePaused(nowTicks);
            break;

        case WRAPPER_WSTATE_RESUMING:
            wStateResuming(nowTicks);
            break;

        case WRAPPER_WSTATE_STOPPING:
            wStateStopping(nowTicks);
            break;

        case WRAPPER_WSTATE_STOPPED:
            wStateStopped(nowTicks);
            break;

        default:
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT("Unknown wState=%d"), wrapperData->wState);
            break;
        }

        /* Do something depending on the JVM state */
        if (wrapperData->isLoopOutputEnabled) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("    Loop: handle JVM state: %s"),
                wrapperGetJState(wrapperData->jState));
        }
        switch(wrapperData->jState) {
        case WRAPPER_JSTATE_DOWN_CLEAN:
            jStateDownClean(nowTicks);
            break;

        case WRAPPER_JSTATE_LAUNCH_DELAY:
            jStateLaunchDelay(nowTicks);
            break;

        case WRAPPER_JSTATE_RESTART:
            jStateRestart(nowTicks);
            break;

        case WRAPPER_JSTATE_LAUNCH:
            jStateLaunch(nowTicks);
            break;

        case WRAPPER_JSTATE_LAUNCHING:
            jStateLaunching(nowTicks);
            break;

        case WRAPPER_JSTATE_LAUNCHED:
            jStateLaunched(nowTicks);
            break;

        case WRAPPER_JSTATE_STARTING:
            jStateStarting(nowTicks);
            break;

        case WRAPPER_JSTATE_STARTED:
            jStateStarted(nowTicks);
            break;

        case WRAPPER_JSTATE_STOP:
            jStateStop(nowTicks);
            break;

        case WRAPPER_JSTATE_STOPPING:
            jStateStopping(nowTicks);
            break;

        case WRAPPER_JSTATE_STOPPED:
            jStateStopped(nowTicks);
            break;

        case WRAPPER_JSTATE_KILLING:
            jStateKilling(nowTicks);
            break;

        case WRAPPER_JSTATE_KILL:
            jStateKill(nowTicks);
            break;

        case WRAPPER_JSTATE_KILLED:
            jStateKillConfirm(nowTicks);
            break;

        case WRAPPER_JSTATE_DOWN_CHECK:
            jStateDownCheck(nowTicks);
            break;

        case WRAPPER_JSTATE_DOWN_FLUSH_STDIN:
            jStateDownFlushStdin(nowTicks);
            break;

        case WRAPPER_JSTATE_DOWN_FLUSH:
            jStateDownFlush(nowTicks);
            break;

        default:
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT("Unknown jState=%d"), wrapperData->jState);
            break;
        }
        
        if ((prevWState != wrapperData->wState) || (prevJState != wrapperData->jState)) {
            /* If either the Wrapper or Java states have changed since the previous cycle, we never want to sleep on the next cycle.
             *  This is done to minimize any delays as the state engine cycles through states. */
            skipSleep = TRUE;
        }
    } while (wrapperData->wState != WRAPPER_WSTATE_STOPPED);

    /* Assertion check of Java State. */
    if (wrapperData->jState != WRAPPER_JSTATE_DOWN_CLEAN) {
        /* Should never appear - no need to localize this message. */
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Wrapper shutting down while java state still %s."), wrapperGetJState(wrapperData->jState));
    }

    if (wrapperData->isLoopOutputEnabled) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("Event loop stopped."));
    }
}
