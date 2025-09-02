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

#ifdef WIN32
 /* need the 2 following includes to use IPv6 and need wsock32.lib in the makefile */
 #include <winsock2.h>
 #include <Ws2tcpip.h>
 #include <psapi.h>
#endif

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/stat.h>
#include <assert.h>

#ifdef CUNIT
#include "CUnit/Basic.h"
#endif
#include "wrapper_i18n.h"
#include "wrapperinfo.h"
#include "wrapper.h"
#include "logger.h"
#include "logger_file.h"
#include "wrapper_jvminfo.h"
#include "wrapper_encoding.h"
#include "wrapper_file.h"
#ifndef WIN32
 #include "wrapper_ulimit.h"
#endif
#include "wrapper_secure_file.h"
#include "wrapper_cipher.h"

#ifdef WIN32
 #include <direct.h>
 #include <winsock.h>
 #include <shlwapi.h>
 #include <windows.h>
#include <io.h>
#include <tlhelp32.h>

/* MS Visual Studio 8 went and deprecated the POXIX names for functions.
 *  Fixing them all would be a big headache for UNIX versions. */
#pragma warning(disable : 4996)

/* Defines for MS Visual Studio 6 */
#ifndef tzname
# define tzname _tzname
#endif
#ifndef timezone
# define timezone _timezone
#endif
#ifndef daylight
# define daylight _daylight
#endif

#ifndef _CRT_SECURE_NO_WARNINGS
# define _CRT_SECURE_NO_WARNINGS 1
#endif

#ifndef _INTPTR_T_DEFINED
  typedef long intptr_t;
  #define _INTPTR_T_DEFINED
#endif

#else /* UNIX */
 #include <ctype.h>
 #include <string.h>
 #include <sys/wait.h>
 #include <fcntl.h>
 #include <limits.h>
 #include <signal.h>
 #include <pthread.h>
 #include <grp.h>
 #include <sys/socket.h>
 #include <sys/time.h>
 #include <netinet/in.h>
 #include <arpa/inet.h>
 #include <sys/resource.h>
 #define SOCKET         int
 #define HANDLE         int
 #define INVALID_HANDLE_VALUE -1
 #define INVALID_SOCKET -1
 #define SOCKET_ERROR   -1

 #if defined(SOLARIS)
  #include <sys/errno.h>
  #include <sys/fcntl.h>
 #elif defined(AIX) || defined(HPUX) || defined(MACOSX)
 #elif defined(FREEBSD)
  #include <sys/param.h>
  #include <errno.h>
 #else /* LINUX */
  #include <asm/errno.h>
 #endif

#endif /* WIN32 */

/* Define some common defines to make cross platform code a bit cleaner. */
#ifdef WIN32
 #define WRAPPER_EADDRINUSE  WSAEADDRINUSE
 #define WRAPPER_EWOULDBLOCK WSAEWOULDBLOCK
 #define WRAPPER_EACCES      WSAEACCES
#else
 #define WRAPPER_EADDRINUSE  EADDRINUSE
 #define WRAPPER_EWOULDBLOCK EWOULDBLOCK
 #define WRAPPER_EACCES      EACCES
#endif

WrapperConfig *wrapperData;
char          packetBufferMB[MAX_LOG_SIZE + 1];
TCHAR         packetBufferW[MAX_LOG_SIZE + 1];
TCHAR         *keyChars = TEXT("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_-");

/* Properties structure loaded in from the configuration file. */
Properties *properties = NULL;
Properties *initialProperties = NULL;

/* Mutex for synchronization of the tick timer. */
#ifdef WIN32
HANDLE tickMutexHandle = NULL;
#else
pthread_mutex_t tickMutex = PTHREAD_MUTEX_INITIALIZER;
#endif

/* Server Pipe Handles. */
HANDLE protocolActiveServerPipeIn = INVALID_HANDLE_VALUE;
HANDLE protocolActiveServerPipeOut = INVALID_HANDLE_VALUE;
#ifndef WIN32
int protocolPipeInFd[2] = { -1, -1 };   /* protocolPipeInFd[0] -> protocolActiveServerPipeIn */
int protocolPipeOuFd[2] = { -1, -1 };   /* protocolPipeOuFd[1] -> protocolActiveServerPipeOut */
#endif

/* Flag for indicating the connected pipes */
int protocolActiveServerPipeConnected = FALSE;

/* Server Socket (it listens to incoming connections, and closes once one is established). */
SOCKET protocolActiveServerSD = INVALID_SOCKET;
/* Client Socket (it accept an incoming connection from the JVM). */
SOCKET protocolActiveBackendSD = INVALID_SOCKET;

#ifndef IN6ADDR_LOOPBACK_INIT
 /* even if I include ws2ipdef.h, it doesn't define IN6ADDR_LOOPBACK_INIT,
    so that's why I define it here */
 #define IN6ADDR_LOOPBACK_INIT { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1 }
#endif
#define LOOPBACK_IPv4  "127.0.0.1"
#ifdef HPUX_IA
 /* on HPUX ia, gcc reports a warning "missing braces around initializer" 
    when using IN6ADDR_LOOPBACK_INIT (/usr/include/netinet/in6.h:241). 
    So I add braces for the struct and braces for the union. */
struct in6_addr LOOPBACK_IPv6 = {{IN6ADDR_LOOPBACK_INIT}};
#else
struct in6_addr LOOPBACK_IPv6 = IN6ADDR_LOOPBACK_INIT;  
#endif

int disposed = FALSE;
int handleSignals = TRUE;

static TCHAR** appParameters = NULL;
static int appParametersLen = 0;

static TCHAR** appProperties = NULL;
static int appPropertiesLen = 0;

static TCHAR** additionals = NULL;
static int additionalsLen = 0;

const TCHAR *wrapperStickyPropertyNames[] = { TEXT("wrapper.anchorfile"),
#ifndef WIN32
                                              TEXT("wrapper.daemonize"),
#else
                                              TEXT("wrapper.javaio.buffer_size"),
#endif
                                              TEXT("wrapper.javaio.use_thread"),
                                              TEXT("wrapper.lockfile"),
                                              TEXT("wrapper.log_buffer_growth"),
#ifdef WIN32
                                              TEXT("wrapper.ntservice.*"),
#endif
                                              TEXT("wrapper.pausable"),
                                              TEXT("wrapper.pausable.stop_jvm"),
                                              TEXT("wrapper.pause_on_startup"),
                                              TEXT("wrapper.pidfile"),
                                              TEXT("wrapper.secure_file.check.*"),
                                              TEXT("wrapper.statusfile"),
                                              TEXT("wrapper.use_javaio_thread"),
                                              TEXT("wrapper.use_system_time"),
                                              TEXT("wrapper.use_tick_mutex"),
                                              TEXT("wrapper.working.dir"),
                                              NULL };

int loadConfiguration();

static char *wrapperChildWorkBuffer = NULL;
static size_t wrapperChildWorkBufferSize = 0;
static size_t wrapperChildWorkBufferLen = 0;
static time_t wrapperChildWorkLastDataTime = 0;
static int wrapperChildWorkLastDataTimeMillis = 0;
static int wrapperChildWorkIsNewLine = TRUE;

/**
 * Constructs a tm structure from a pair of Strings like "20091116" and "1514".
 *  The time returned will be in the local time zone.  This is not 100% accurate
 *  as it doesn't take into account the time zone in which the dates were
 *  originally set.
 */
struct tm getInfoTime(const TCHAR *date, const TCHAR *time) {
    struct tm buildTM;
    TCHAR temp[5];

    memset(&buildTM, 0, sizeof(struct tm));

    /* Year */
    _tcsncpy( temp, date, 4 );
    temp[4] = 0;
    buildTM.tm_year = _ttoi( temp ) - 1900;

    /* Month */
    _tcsncpy( temp, date + 4, 2 );
    temp[2] = 0;
    buildTM.tm_mon = _ttoi( temp ) - 1;

    /* Day */
    _tcsncpy( temp, date + 6, 2 );
    temp[2] = 0;
    buildTM.tm_mday = _ttoi( temp );

    /* Hour */
    _tcsncpy( temp, time, 2 );
    temp[2] = 0;
    buildTM.tm_hour = _ttoi( temp );

    /* Minute */
    _tcsncpy( temp, time + 2, 2 );
    temp[2] = 0;
    buildTM.tm_min = _ttoi( temp );

    return buildTM;
}

struct tm wrapperGetReleaseTime() {
    return getInfoTime(wrapperReleaseDate, wrapperReleaseTime);
}

struct tm wrapperGetBuildTime() {
    return getInfoTime(wrapperBuildDate, wrapperBuildTime);
}

/**
 * Adds default properties used to set global environment variables.
 *
 * These are done by setting properties rather than call setEnv directly
 *  so that it will be impossible for users to override their values by
 *  creating a "set.XXX=NNN" property in the configuration file.
 */
void wrapperAddDefaultProperties(Properties *props) {
    TCHAR buffer[11]; /* should be large enough to contain the pid and lang (increase the buffer size if more variables are needed) */
    TCHAR* confDirTemp;
#ifdef WIN32
    int work, pos2;
    TCHAR pathSep = TEXT('\\');
#else
    TCHAR pathSep = TEXT('/');
#endif
    int pos;
#ifdef WIN32
    const TCHAR* fileSeparator = TEXT("\\");
    const TCHAR* pathSeparator = TEXT(";");
#else
    const TCHAR* fileSeparator = TEXT("/");
    const TCHAR* pathSeparator = TEXT(":");
#endif

    if (wrapperData->confDir == NULL) {
        if (_tcsrchr(wrapperData->argConfFile, pathSep) != NULL) {
            pos = (int)(_tcsrchr(wrapperData->argConfFile, pathSep) - wrapperData->argConfFile);
        } else {
            pos = -1;
        }
#ifdef WIN32
        if (_tcsrchr(wrapperData->argConfFile, TEXT('/')) != NULL) {
            pos2 = (int)(_tcsrchr(wrapperData->argConfFile, TEXT('/')) - wrapperData->argConfFile);
        } else {
            pos2 = -1;
        }
        pos = __max(pos, pos2);
#endif
        if (pos == -1) {
            confDirTemp = malloc(sizeof(TCHAR) * 2);
            if (!confDirTemp) {
                outOfMemory(TEXT("WADP"), 1);
                return;
            }
            _tcsncpy(confDirTemp, TEXT("."), 2);
        } else if (pos == 0) {
            confDirTemp = malloc(sizeof(TCHAR) * 2);
            if (!confDirTemp) {
                outOfMemory(TEXT("WADP"), 2);
                return;
            }
            _sntprintf(confDirTemp, 2, TEXT("%c"), pathSep);
        } else {
            confDirTemp = malloc(sizeof(TCHAR) * (pos + 1));
            if (!confDirTemp) {
                outOfMemory(TEXT("WADP"), 3);
                return;
            }
            _tcsncpy(confDirTemp, wrapperData->argConfFile, pos);
            confDirTemp[pos] = TEXT('\0');
        }
#ifdef WIN32
        /* Get buffer size, including '\0' */
        work = GetFullPathName(confDirTemp, 0, NULL, NULL);
        if (!work) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL,
                TEXT("Unable to resolve the conf directory: %s"), getLastErrorText());
            free(confDirTemp);
            return;
        }
        wrapperData->confDir = malloc(sizeof(TCHAR) * work);
        if (!wrapperData->confDir) {
            outOfMemory(TEXT("WADP"), 4);
            free(confDirTemp);
            return;
        }
        if (!GetFullPathName(confDirTemp, work, wrapperData->confDir, NULL)) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL,
                TEXT("Unable to resolve the conf directory: %s"), getLastErrorText());
            free(confDirTemp);
            return;
        }
#else
        /* The solaris implementation of realpath will return a relative path if a relative
         *  path is provided.  We always need an absolute path here.  So build up one and
         *  then use realpath to remove any .. or other relative references. */
        wrapperData->confDir = malloc(sizeof(TCHAR) * (PATH_MAX + 1));
        if (!wrapperData->confDir) {
            outOfMemory(TEXT("WADP"), 5);
            free(confDirTemp);
            return;
        }
        if (_trealpathN(confDirTemp, wrapperData->confDir, PATH_MAX + 1) == NULL) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL,
                TEXT("Unable to resolve the original working directory: %s"), getLastErrorText());
            free(confDirTemp);
            return;
        }
#endif
        setEnv(TEXT("WRAPPER_CONF_DIR"), wrapperData->confDir, ENV_SOURCE_APPLICATION);
        free(confDirTemp);
    }

    _sntprintf(buffer, 3, TEXT("en"));
    setInternalVarProperty(props, TEXT("WRAPPER_LANG"), buffer, TRUE, FALSE);
    _sntprintf(buffer, 11, TEXT("%d"), wrapperData->wrapperPID);
    setInternalVarProperty(props, TEXT("WRAPPER_PID"), buffer, TRUE, FALSE);
    setInternalVarProperty(props, TEXT("WRAPPER_BASE_NAME"), wrapperData->baseName, TRUE, FALSE);
    setInternalVarProperty(props, TEXT("WRAPPER_BITS"), wrapperBits, TRUE, FALSE);
    setInternalVarProperty(props, TEXT("WRAPPER_ARCH"), wrapperArch, TRUE, FALSE);
    setInternalVarProperty(props, TEXT("WRAPPER_OS"), wrapperOS, TRUE, FALSE);
    setInternalVarProperty(props, TEXT("WRAPPER_VERSION"), wrapperVersionRoot, TRUE, FALSE);
    setInternalVarProperty(props, TEXT("WRAPPER_EDITION"), TEXT("Community"), TRUE, FALSE);
    setInternalVarProperty(props, TEXT("WRAPPER_HOSTNAME"), wrapperData->hostName, TRUE, FALSE);
    setInternalVarProperty(props, TEXT("WRAPPER_HOST_NAME"), wrapperData->hostName, TRUE, FALSE);
    setInternalVarProperty(props, TEXT("WRAPPER_RUN_MODE"), wrapperData->isConsole ? TEXT("console") : TEXT("service"), TRUE, FALSE);
    setInternalVarProperty(props, TEXT("WRAPPER_FILE_SEPARATOR"), fileSeparator, TRUE, FALSE);
    setInternalVarProperty(props, TEXT("WRAPPER_PATH_SEPARATOR"), pathSeparator, TRUE, FALSE);
    /* These variables don't need be set as an environment variables, but we should still register them to prevent users from defining them on their own. */
    setInternalVarProperty(props, TEXT("WRAPPER_PERCENTAGE"), NULL, TRUE, FALSE);
    setInternalVarProperty(props, TEXT("WRAPPER_RAND_N"), NULL, TRUE, FALSE);
    setInternalVarProperty(props, TEXT("WRAPPER_RAND_NN"), NULL, TRUE, FALSE);
    setInternalVarProperty(props, TEXT("WRAPPER_RAND_NNN"), NULL, TRUE, FALSE);
    setInternalVarProperty(props, TEXT("WRAPPER_RAND_NNNN"), NULL, TRUE, FALSE);
    setInternalVarProperty(props, TEXT("WRAPPER_RAND_NNNNN"), NULL, TRUE, FALSE);
    setInternalVarProperty(props, TEXT("WRAPPER_RAND_NNNNNN"), NULL, TRUE, FALSE);
    /* NOTE: currently possible for the user to set WRAPPER_SYSTEM_<P>. In order to block it we would need to use another method because there is no fixed name
     *  that we can add to the property list. One idea is to create a separated list of wildcarded properties and a method to loop over them checking that there
     *  is no match before adding a user-defined property to the real list of properties. */
    setInternalVarProperty(props, TEXT("WRAPPER_TIME_YYYYMMDDHHIISS"), NULL, TRUE, FALSE);
    setInternalVarProperty(props, TEXT("WRAPPER_TIME_YYYYMMDD_HHIISS"), NULL, TRUE, FALSE);
    setInternalVarProperty(props, TEXT("WRAPPER_TIME_YYYYMMDDHHII"), NULL, TRUE, FALSE);
    setInternalVarProperty(props, TEXT("WRAPPER_TIME_YYYYMMDDHH"), NULL, TRUE, FALSE);
    setInternalVarProperty(props, TEXT("WRAPPER_TIME_YYYYMMDD"), NULL, TRUE, FALSE);
    setInternalVarProperty(props, TEXT("WRAPPER_JAVA_VERSION"), NULL, FALSE, FALSE);
    setInternalVarProperty(props, TEXT("WRAPPER_JAVA_VERSION_MAJOR"), NULL, FALSE, FALSE);
    setInternalVarProperty(props, TEXT("WRAPPER_JAVA_VERSION_MINOR"), NULL, FALSE, FALSE);
    setInternalVarProperty(props, TEXT("WRAPPER_JAVA_VERSION_REVISION"), NULL, FALSE, FALSE);
    setInternalVarProperty(props, TEXT("WRAPPER_JAVA_VENDOR"), NULL, FALSE, FALSE);
#ifdef WIN32
    /* Do not change the value of this variable as this would cause a memory leak on each JVM restart (see setEnvInner()). */
    if (wrapperData->registry_java_home) {
        setInternalVarProperty(props, TEXT("WRAPPER_JAVA_HOME"), wrapperData->registry_java_home, FALSE, TRUE);
    } else {
        setInternalVarProperty(props, TEXT("WRAPPER_JAVA_HOME"), NULL, FALSE, FALSE);
    }
#endif
    /* The following variables are never set as environment variables (no memory leak should happen). */
    setInternalVarProperty(props, TEXT("WRAPPER_NAME"), NULL, TRUE, FALSE);
    setInternalVarProperty(props, TEXT("WRAPPER_DISPLAYNAME"), NULL, TRUE, FALSE);
    setInternalVarProperty(props, TEXT("WRAPPER_DESCRIPTION"), NULL, TRUE, FALSE);
    setInternalVarProperty(props, TEXT("WRAPPER_EVENT_JVM_PID"), NULL, TRUE, FALSE);
    setInternalVarProperty(props, TEXT("WRAPPER_EVENT_JVM_ID"), NULL, TRUE, FALSE);
    setInternalVarProperty(props, TEXT("WRAPPER_EVENT_NAME"), NULL, TRUE, FALSE);
    setInternalVarProperty(props, TEXT("WRAPPER_EVENT_RAND_N"), NULL, TRUE, FALSE);
    setInternalVarProperty(props, TEXT("WRAPPER_EVENT_RAND_NN"), NULL, TRUE, FALSE);
    setInternalVarProperty(props, TEXT("WRAPPER_EVENT_RAND_NNN"), NULL, TRUE, FALSE);
    setInternalVarProperty(props, TEXT("WRAPPER_EVENT_RAND_NNNN"), NULL, TRUE, FALSE);
    setInternalVarProperty(props, TEXT("WRAPPER_EVENT_RAND_NNNNN"), NULL, TRUE, FALSE);
    setInternalVarProperty(props, TEXT("WRAPPER_EVENT_RAND_NNNNNN"), NULL, TRUE, FALSE);
    setInternalVarProperty(props, TEXT("WRAPPER_EVENT_TIME_YYYYMMDDHHIISS"), NULL, TRUE, FALSE);
    setInternalVarProperty(props, TEXT("WRAPPER_EVENT_TIME_YYYYMMDD_HHIISS"), NULL, TRUE, FALSE);
    setInternalVarProperty(props, TEXT("WRAPPER_EVENT_TIME_YYYYMMDDHHII"), NULL, TRUE, FALSE);
    setInternalVarProperty(props, TEXT("WRAPPER_EVENT_TIME_YYYYMMDDHH"), NULL, TRUE, FALSE);
    setInternalVarProperty(props, TEXT("WRAPPER_EVENT_TIME_YYYYMMDD"), NULL, TRUE, FALSE);
    setInternalVarProperty(props, TEXT("WRAPPER_EVENT_WRAPPER_PID"), NULL, TRUE, FALSE);
}

/**
 * This function is here to help Community Edition users who are attempting
 *  to generate a hostId.
 */
int showHostIds(int logLevel, int notUsed) {
    log_printf(WRAPPER_SOURCE_WRAPPER, logLevel, TEXT(""));
    log_printf(WRAPPER_SOURCE_WRAPPER, logLevel, TEXT("The Community Edition of the Java Service Wrapper does not implement\nHostIds."));
    log_printf(WRAPPER_SOURCE_WRAPPER, logLevel, TEXT(""));
    log_printf(WRAPPER_SOURCE_WRAPPER, logLevel, TEXT("If you have requested a trial license, or purchased a license, you\nmay be looking for the Standard or Professional Editions of the Java\nService Wrapper.  They can be downloaded here:"));
    log_printf(WRAPPER_SOURCE_WRAPPER, logLevel, TEXT("  https://wrapper.tanukisoftware.com/download"));
    log_printf(WRAPPER_SOURCE_WRAPPER, logLevel, TEXT(""));

    return FALSE;
}

/**
 * Attempt to set the console title if it exists and is accessible.
 */
void wrapperSetConsoleTitle() {
#ifdef WIN32
    if (wrapperData->consoleTitle) {
        if (wrapperProcessHasVisibleConsole()) {
            /* The console should be visible. */
            if (!SetConsoleTitle(wrapperData->consoleTitle)) {
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN,
                    TEXT("Attempt to set the console title failed: %s"), getLastErrorText());
            }
        }
    }
#elif LINUX
    /*  This works on all UNIX versions, but only Linux resets it
     *  correctly when the wrapper process terminates. */
    if (wrapperData->consoleTitle) {
        if (wrapperData->isConsole) {
            /* The console should be visible. */
            _tprintf(TEXT("%c]0;%s%c"), TEXT('\033'), wrapperData->consoleTitle, TEXT('\007'));
        }
    }
#endif
}

/**
 * Loads the current environment into a table so we can debug it later.
 *
 * @return TRUE if there were problems, FALSE if successful.
 */
int loadEnvironment() {
    size_t len;
    TCHAR *sourcePair;
    TCHAR *pair;
    TCHAR *equal;
    TCHAR *name;
    TCHAR *value;
#ifdef WIN32
    LPTCH lpvEnv;
    LPTSTR lpszVariable;
#else
    /* The compiler won't let us reference environ directly in the for loop on OSX because it is actually a function. */
    char **environment = environ;
    int i;
#endif

#ifdef _DEBUG
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("Loading Environment..."));
#endif
#ifdef WIN32
    lpvEnv = GetEnvironmentStrings();
    if (!lpvEnv)
    {
        _tprintf(TEXT("GetEnvironmentStrings failed (%s)\n"), getLastErrorText());
        return TRUE;
    }
    lpszVariable = (LPTSTR)lpvEnv;
    while (lpszVariable[0] != '\0') {
            sourcePair = lpszVariable;
#else
    i = 0;
    while (environment[i]) {
        len = mbstowcs(NULL, environment[i], MBSTOWCS_QUERY_LENGTH);
        if (len == (size_t)-1) {
            /* Invalid string.  Skip. */
        } else {
            sourcePair = malloc(sizeof(TCHAR) * (len + 1));
            if (!sourcePair) {
                outOfMemory(TEXT("LE"), 1);
                _tprintf(TEXT(" Invalid character string: %s (%s)\n"), environment[i], getLastErrorText());
                return TRUE;
            }
            mbstowcs(sourcePair, environment[i], len + 1);
            sourcePair[len] = TEXT('\0'); /* Avoid bufferflows caused by badly encoded characters. */
#endif

            len = _tcslen(sourcePair);

            /* We need a copy of the variable pair so we can split it. */
            pair = malloc(sizeof(TCHAR) * (len + 1));
            if (!pair) {
                outOfMemory(TEXT("LE"), 1);
#ifdef WIN32
                FreeEnvironmentStrings(lpvEnv);
#else
                free(sourcePair);
#endif
                return TRUE;
            }
            _sntprintf(pair, len + 1, TEXT("%s"), sourcePair);

            equal = _tcschr(pair, TEXT('='));
            if (equal) {
                name = pair;
                value = &(equal[1]);
                equal[0] = TEXT('\0');

                if (_tcslen(name) <= 0) {
                    name = NULL;
                }
                if (_tcslen(value) <= 0) {
                    value = NULL;
                }

                /* It is possible that the name was empty. */
                if (name) {
                    setEnv(name, value, ENV_SOURCE_PARENT);
                }
            }

            free(pair);

#ifdef WIN32
            lpszVariable += len + 1;
#else
            free(sourcePair);
        }
        i++;
#endif
    }
#ifdef WIN32
    FreeEnvironmentStrings(lpvEnv);
#endif

#ifdef _DEBUG
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("Loading Environment complete."));
#endif
    return FALSE;
}

#ifndef WIN32 /* UNIX */
int getSignalMode(const TCHAR *modeName, int defaultMode) {
    if (!modeName) {
        return defaultMode;
    }

    if (strcmpIgnoreCase(modeName, TEXT("IGNORE")) == 0) {
        return WRAPPER_SIGNAL_MODE_IGNORE;
    } else if (strcmpIgnoreCase(modeName, TEXT("RESTART")) == 0) {
        return WRAPPER_SIGNAL_MODE_RESTART;
    } else if (strcmpIgnoreCase(modeName, TEXT("SHUTDOWN")) == 0) {
        return WRAPPER_SIGNAL_MODE_SHUTDOWN;
    } else if (strcmpIgnoreCase(modeName, TEXT("FORWARD")) == 0) {
        return WRAPPER_SIGNAL_MODE_FORWARD;
    } else if (strcmpIgnoreCase(modeName, TEXT("PAUSE")) == 0) {
        return WRAPPER_SIGNAL_MODE_PAUSE;
    } else if (strcmpIgnoreCase(modeName, TEXT("RESUME")) == 0) {
        return WRAPPER_SIGNAL_MODE_RESUME;
    } else if (strcmpIgnoreCase(modeName, TEXT("CLOSE_LOGFILE")) == 0) {
        return WRAPPER_SIGNAL_MODE_CLOSE_LOGFILE;
    } else {
        return defaultMode;
    }
}

void wrapperBuildUnixDaemonInfo() {
    if (!wrapperData->configured) {
        /** Configure the HUP signal handler. */
        wrapperData->signalHUPMode = getSignalMode(getStringProperty(properties, TEXT("wrapper.signal.mode.hup"), NULL), WRAPPER_SIGNAL_MODE_FORWARD);

        /** Configure the USR1 signal handler. */
        wrapperData->signalUSR1Mode = getSignalMode(getStringProperty(properties, TEXT("wrapper.signal.mode.usr1"), NULL), WRAPPER_SIGNAL_MODE_FORWARD);

        /** Configure the USR2 signal handler. */
        wrapperData->signalUSR2Mode = getSignalMode(getStringProperty(properties, TEXT("wrapper.signal.mode.usr2"), NULL), WRAPPER_SIGNAL_MODE_FORWARD);
    }
}
#endif

void setEnvironmentLogLevel(int logLevel) {
    wrapperData->environmentLogLevel = logLevel;
}

/**
 * Dumps the table of environment variables, and their sources.
 */
void dumpEnvironment() {
    EnvSrc *envSrc;
    TCHAR *envVal;
    int logLevel = wrapperData->environmentLogLevel;
    const TCHAR* ignore;

    log_printf(WRAPPER_SOURCE_WRAPPER, logLevel, TEXT(""));
    log_printf(WRAPPER_SOURCE_WRAPPER, logLevel, TEXT("Environment variables (Source | Name=Value) BEGIN:"));

    envSrc = baseEnvSrc;
    while (envSrc) {
        /* Do not display the variables that are not expanded. */
        if (properties->ignoreVarMap) {
            /* Can return NULL if missing or "TRUE" or "FALSE". */
            ignore = hashMapGetKWVW(properties->ignoreVarMap, envSrc->name);
        } else {
            ignore = NULL;
        }
        if (!ignore || strcmpIgnoreCase(ignore, TEXT("TRUE")) != 0) {
            envVal = _tgetenv(envSrc->name);

            log_printf(WRAPPER_SOURCE_WRAPPER, logLevel, TEXT("  %c%c%c%c%c | %s=%s"),
                (envSrc->source & ENV_SOURCE_PARENT ? TEXT('P') : TEXT('-')),
#ifdef WIN32
                (envSrc->source & ENV_SOURCE_REG_SYSTEM ? TEXT('S') : TEXT('-')),
                (envSrc->source & ENV_SOURCE_REG_ACCOUNT ? TEXT('A') : TEXT('-')),
#else
                TEXT('-'),
                TEXT('-'),
#endif
                (envSrc->source & ENV_SOURCE_APPLICATION ? TEXT('W') : TEXT('-')),
                (envSrc->source & ENV_SOURCE_CONFIG ? TEXT('C') : TEXT('-')),
                envSrc->name,
                (envVal ? envVal : TEXT("<null>"))
            );

#if !defined(WIN32) && defined(UNICODE)
            if (envVal) {
                free(envVal);
            }
#endif
        }
        envSrc = envSrc->next;
    }
    log_printf(WRAPPER_SOURCE_WRAPPER, logLevel, TEXT("Environment variables END:"));
    log_printf(WRAPPER_SOURCE_WRAPPER, logLevel, TEXT(""));
}

#ifdef WIN32
/**
 * Check if the Wrapper is running under cygwin terminal.
 * I'm looking for the environment variable TERM to be equal to "xterm".
 * I tried with OSTYPE and MACHTYPE, but _tgetenv always returns NULL.
 * @return TRUE if under cygwin, otherwise returns FALSE
 */
int isCygwin() {
    TCHAR *osType;
    int retVal = FALSE;
    
    osType = _tgetenv(TEXT("TERM"));
    
    if ((osType != NULL) && (_tcscmp(osType, TEXT("xterm")) == 0)) {
        retVal = TRUE;
    } 

    return retVal;
}
#endif

/**
 * Return TRUE if the this is a prompt call made from the script (like --translate or --jvm_bits or --request_delta_binary_bits or --request_log_file or --request_default_log_file).
 *
 * @param argCommand the first arguement passed when launching the Wrapper
 */
int isPromptCallCommand(const TCHAR* argCommand) {
    if (!argCommand) {
        return FALSE;
    }
    return ((strcmpIgnoreCase(argCommand, TEXT("-translate")) == 0) ||
            (strcmpIgnoreCase(argCommand, TEXT("-jvm_bits")) == 0) ||
#ifndef WIN32
            (strcmpIgnoreCase(argCommand, TEXT("-request_log_file")) == 0) ||
            (strcmpIgnoreCase(argCommand, TEXT("-request_default_log_file")) == 0) ||
#endif
            (strcmpIgnoreCase(argCommand, TEXT("-request_delta_binary_bits")) == 0));
}

/**
 * Return TRUE if the this is a prompt call made from the script (like --translate or --jvm_bits or --request_delta_binary_bits or --request_log_file or --request_default_log_file).
 *  This function must be called after the arguments have been parsed!
 */
int isPromptCall() {
    return isPromptCallCommand(wrapperData->argCommand);
}

int wrapperLoadLoggingProperties(int preload) {
    const TCHAR *logfilePath;
    int noLogFile;
    int logfileRollMode;
    int defaultFlushTimeOut = 1;
    int loglevelTargetsSet = FALSE;
#ifdef WIN32
    int consoleDirect;
    int silent;
#endif
    int isPurgePatternGenerated = FALSE;
    const TCHAR* confPurgePattern;
    
    setLoggingIsPreload(preload);
    
    setLogPropertyWarnings(properties, !preload);
    
    setLogPropertyWarningLogLevel(properties, getLogLevelForName(getStringProperty(properties, TEXT("wrapper.property_warning.loglevel"), TEXT("WARN"))));
    
    setPropertiesDumpLogLevel(properties, getLogLevelForName(getStringProperty(properties, TEXT("wrapper.properties.dump.loglevel"), TEXT("DEBUG"))));
    
    setPropertiesDumpFormat(properties, getStringProperty(properties, TEXT("wrapper.properties.dump.format"), PROPERTIES_DUMP_FORMAT_DEFAULT));
    
    setEnvironmentLogLevel(getLogLevelForName(getStringProperty(properties, TEXT("wrapper.environment.dump.loglevel"), getBooleanProperty(properties, TEXT("wrapper.environment.dump"), FALSE) ? TEXT("INFO") : TEXT("DEBUG"))));

    setLogWarningThreshold(getIntProperty(properties, TEXT("wrapper.log.warning.threshold"), 0));
    wrapperData->logLFDelayThreshold = propIntMax(propIntMin(getIntProperty(properties, TEXT("wrapper.log.lf_delay.threshold"), 500), 3600000), 0);

    if (resolveDefaultLogFilePath()) {
        /* The error has already been logged. This is not fatal, we will continue with the relative path. */
    }
    
    logfilePath = getFileSafeStringProperty(properties, TEXT("wrapper.logfile"), TEXT("wrapper.log"));
    if (setLogfilePath(logfilePath, TRUE, preload)) {
        return TRUE;
    }

    noLogFile = (_tcslen(logfilePath) == 0) ? TRUE : FALSE;

#ifdef WIN32
    if (!noLogFile && isLauncherMode() && !isNonInteractiveSession() && getBooleanProperty(properties, TEXT("wrapper.wrapperm.logfile.enable"), FALSE) == FALSE) {
        setLogfileLevelInt(LEVEL_NONE);
        noLogFile = TRUE;
    }
#endif

    if (!noLogFile) {
        /* When the log file doesn't exist or is completely disabled, skip loading its configuration to avoid any irrelevent warnings.
         *  Note that the configuration must be loaded in case a log file is configured but its log level has been set to NONE,
         *  as there may be situations where this level will be changed later (e.g. using of LOGFILE_LOGLEVEL in the command file). */
        logfileRollMode = getLogfileRollModeForName(getStringProperty(properties, TEXT("wrapper.logfile.rollmode"), TEXT("SIZE")));
        if (logfileRollMode == ROLL_MODE_UNKNOWN) {
            if (!preload) {
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN,
                TEXT("wrapper.logfile.rollmode invalid.  Disabling log file rolling."));
            }
            logfileRollMode = ROLL_MODE_NONE;
        } else if (logfileRollMode == ROLL_MODE_DATE) {
            if (!_tcsstr(logfilePath, ROLL_MODE_DATE_TOKEN)) {
                if (!preload) {
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN,
                        TEXT("wrapper.logfile must contain \"%s\" for a roll mode of DATE.  Disabling log file rolling."),
                        ROLL_MODE_DATE_TOKEN);
                }
                logfileRollMode = ROLL_MODE_NONE;
            }
        }
        setLogfileRollMode(logfileRollMode);

        /* Load log file format */
        setLogfileFormat(getStringProperty(properties, TEXT("wrapper.logfile.format"), LOG_FORMAT_LOGFILE_DEFAULT));

        /* Load log file log level */
        setLogfileLevel(getStringProperty(properties, TEXT("wrapper.logfile.loglevel"), TEXT("INFO")));

        /* Load max log filesize log level */
        setLogfileMaxFileSize(getStringProperty(properties, TEXT("wrapper.logfile.maxsize"), TEXT("0")));

        /* Load log files level */
        setLogfileMaxLogFiles(getIntProperty(properties, TEXT("wrapper.logfile.maxfiles"), 0));

        /* Load log file purge sort */
        setLogfilePurgeSortMode(loggerFileGetSortMode(getStringProperty(properties, TEXT("wrapper.logfile.purge.sort"), TEXT("NAMES_SMART"))));

        /* Load log file purge pattern */
        confPurgePattern = getFileSafeStringProperty(properties, TEXT("wrapper.logfile.purge.pattern"), TEXT(""));
        setLogfilePurgePattern(confPurgePattern, &isPurgePatternGenerated);

        if (preload) {
            if (!strcmpIgnoreCase(wrapperData->argCommand, TEXT("c")) || !strcmpIgnoreCase(wrapperData->argCommand, TEXT("-console")) ||
                !strcmpIgnoreCase(wrapperData->argCommand, TEXT("s")) || !strcmpIgnoreCase(wrapperData->argCommand, TEXT("-service"))) {
                /* See if the logs should be rolled on Wrapper startup (console and service only).
                 *  This is done once during preload so that there is no output before the file is being rolled. */
                if ((getLogfileRollMode() & ROLL_MODE_WRAPPER) ||
                    (getLogfileRollMode() & ROLL_MODE_JVM)) {
                    rollLogs(NULL);
                }
            }
        }

        /* Get the close timeout. */
#ifdef WIN32
        if (isLauncherMode()) {
            /* For launcher instances (wrapperm), always close the log file after each log entry to not
             *  risk blocking it for the running service instance, otherwise it may prevent log rolling. */
            wrapperData->logfileCloseTimeout = 0;
        } else {
#endif
            wrapperData->logfileCloseTimeout = propIntMax(propIntMin(getIntProperty(properties, TEXT("wrapper.logfile.close.timeout"), getIntProperty(properties, TEXT("wrapper.logfile.inactivity.timeout"), 1)), 3600), -1);
#ifdef WIN32
        }
#endif
        setLogfileAutoClose(wrapperData->logfileCloseTimeout == 0);

        if (!preload && (confPurgePattern[0] != 0) && isPurgePatternGenerated) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN, TEXT("Ignoring the value of wrapper.logfile.purge.pattern because\n  wrapper.logfile.purge.sort was set to 'NAMES_SMART'."));
        }
    }

    /* Get the flush timeout. */
    /* We should stay autoflush until the main loop is reached. Keep the value for later. */
    wrapperData->logfileFlushTimeout = propIntMax(propIntMin(getIntProperty(properties, TEXT("wrapper.logfile.flush.timeout"), defaultFlushTimeOut), 3600), 0);

    /* Load console format */
    setConsoleLogFormat(getStringProperty(properties, TEXT("wrapper.console.format"), LOG_FORMAT_CONSOLE_DEFAULT));

    /* Load console log level */
    setConsoleLogLevel(getStringProperty(properties, TEXT("wrapper.console.loglevel"), TEXT("INFO")));

    /* Load the console direct flag & console flush flag. */
#ifdef WIN32
    consoleDirect = getBooleanProperty(properties, TEXT("wrapper.console.direct"), TRUE);
    if (isSecondary()) {
        setConsoleDirect(FALSE);
        setConsoleFlush(TRUE);
        if (consoleDirect) {
            /* When piping stdout and stderr from the child to the parent process,
             *  the order of the messages between the 2 streams can't be guaranted.
             * - If wrapper.console.direct was set to TRUE, which is the default,
             *   force using only stdout to avoid this issue.
             * - If wrapper.console.direct was set to FALSE, it's most likely that
             *   the user wants to catch each stream separately, so the order
             *   should not matter. */
            setConsoleFatalToStdErr(FALSE);
            setConsoleErrorToStdErr(FALSE);
            setConsoleWarnToStdErr(FALSE);
            loglevelTargetsSet = TRUE;
        }
    } else {
        setConsoleDirect(consoleDirect);
        setConsoleFlush(getBooleanProperty(properties, TEXT("wrapper.console.flush"), isCygwin()));
    }
#else
    setConsoleFlush(getBooleanProperty(properties, TEXT("wrapper.console.flush"), FALSE));
#endif

    /* Load the console loglevel targets. */
    if (!loglevelTargetsSet) {
        setConsoleFatalToStdErr(getBooleanProperty(properties, TEXT("wrapper.console.fatal_to_stderr"), TRUE));
        setConsoleErrorToStdErr(getBooleanProperty(properties, TEXT("wrapper.console.error_to_stderr"), TRUE));
        setConsoleWarnToStdErr(getBooleanProperty(properties, TEXT("wrapper.console.warn_to_stderr"), FALSE));
    }

    /* Get the debug status (Property is deprecated but flag is still used) */
    wrapperData->isDebugging = getBooleanProperty(properties, TEXT("wrapper.debug"), FALSE);
    if (wrapperData->isDebugging) {
        /* For backwards compatability */
        setConsoleLogLevelInt(LEVEL_DEBUG);
        setLogfileLevelInt(LEVEL_DEBUG);
    } else {
        if (getLowLogLevel() <= LEVEL_DEBUG) {
            wrapperData->isDebugging = TRUE;
        }
    }
    
    /* Get the adviser status */
    wrapperData->isAdviserEnabled = getBooleanProperty(properties, TEXT("wrapper.adviser"), TRUE);
    /* The adviser is always enabled if debug is enabled. */
    if (wrapperData->isDebugging) {
        wrapperData->isAdviserEnabled = TRUE;
    }

    /* Load syslog log level */
#ifdef WIN32
    if (isLauncherMode() && !isNonInteractiveSession() && getBooleanProperty(properties, TEXT("wrapper.wrapperm.syslog.enable"), FALSE) == FALSE) {
        setSyslogLevelInt(LEVEL_NONE);
    } else {
#endif
        setSyslogLevel(getStringProperty(properties, TEXT("wrapper.syslog.loglevel"), TEXT("NONE")));
#ifdef WIN32
    }
#endif
    
    /* Load syslog split messages flag. */
    setSyslogSplitMessages(getBooleanProperty(properties, TEXT("wrapper.syslog.split_messages"), FALSE));

#ifndef WIN32
    /* Load syslog facility */
    setSyslogFacility(getStringProperty(properties, TEXT("wrapper.syslog.facility"), TEXT("USER")));
#endif

    /* Load syslog event source name */
    setSyslogEventSourceName(getStringProperty(properties, TEXT("wrapper.syslog.ident"), getStringProperty(properties, TEXT("wrapper.name"), getStringProperty(properties, TEXT("wrapper.ntservice.name"), TEXT("wrapper")))));
#ifdef WIN32
    setSyslogRegister(getBooleanProperty(properties, TEXT("wrapper.syslog.ident.enable"), TRUE));
#endif


#ifdef WIN32
    /* Register or unregister an event source depending on the value of wrapper.syslog.ident.enable.
     *  The syslog will be disabled if the application is not registered after calling the functions
     *  to register or unregister, so this has to be done on preload. This has to be done again each
     *  time the configuration is reloaded because wrapper.syslog.ident.enable may possibly change. */
    /* Make sure we are not running in setup, teardown or install mode. 
     *  - Setup is automatically executed when installing a service - no need to do it here.
     *  - TearDown is not executed when removing the service as this would remove the source 
     *    of existing messages in the event log. */
    if (strcmpIgnoreCase(wrapperData->argCommand, TEXT("su")) && strcmpIgnoreCase(wrapperData->argCommand, TEXT("-setup")) &&
        strcmpIgnoreCase(wrapperData->argCommand, TEXT("td")) && strcmpIgnoreCase(wrapperData->argCommand, TEXT("-teardown")) &&
        strcmpIgnoreCase(wrapperData->argCommand, TEXT("i"))  && strcmpIgnoreCase(wrapperData->argCommand, TEXT("-install")) &&
        strcmpIgnoreCase(wrapperData->argCommand, TEXT("it")) && strcmpIgnoreCase(wrapperData->argCommand, TEXT("-installstart"))) {
        /* The functions below need to be called even if we don't have the permission to edit the registry.
         *  They will eventually disable event logging if the application is not registered.
         *  On preload use the silent mode to avoid double log outputs. */
        silent = preload || !strcmpIgnoreCase(wrapperData->argCommand, TEXT("r")) || !strcmpIgnoreCase(wrapperData->argCommand, TEXT("-remove"))
                         || !strcmpIgnoreCase(wrapperData->argCommand, TEXT("p")) || !strcmpIgnoreCase(wrapperData->argCommand, TEXT("-stop"));
        if (getSyslogRegister()) {
            /* Register the syslog message */
            registerSyslogMessageFile(FALSE, silent);
        } else {
            /* Unregister the syslog message */
            unregisterSyslogMessageFile(silent);
        }
    }
#endif

    return FALSE;
}

/**
 * Retrieve the configured exit code that should be returned when the Wrapper ends with an error.
 *  This function should be called after the configuration has been loaded (after the logging
 *  has been loaded if silent is FALSE).
 *
 * @param silent TRUE if log output should be disabled.
 */
void getConfiguredErrorExitCode(int silent) {
    wrapperData->errorExitCode = getIntProperty(properties, TEXT("wrapper.exit_code.error"), 1);
    if (wrapperData->errorExitCode < 1 || wrapperData->errorExitCode > 255) {
        wrapperData->errorExitCode = 1;
        if (!silent) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN,
                TEXT("%s must be in the range %d to %d.  Changing to %d."), TEXT("wrapper.exit_code.error"), 1, 255, 1);
        }
    }
}

/**
 * This function provides a log file after proloading the properties.
 *  It will load all configurations related to the logging (loglevel, format, etc.).
 *  For standard editions, it helps us to resolve the language, if specified in the conf file.
 *
 * @param logLevelOnOverwriteProperties : used to keep the value of the last #properties.on_overwrite.loglevel found during the preload phase (this is needed for command line properties)
 * @param exitOnOverwriteProperties     : used to keep the value of the last #properties.on_overwrite.exit found during the preload phase (this is needed for command line properties)
 *
 * @return TRUE if there was a FATAL error which will not be reported again during the second load. FALSE otherwise.
 *         It is prefered to return FALSE and log the localized error on the second load if we know it will reappear.
 */
int wrapperPreLoadConfigurationProperties(int *logLevelOnOverwriteProperties, int *exitOnOverwriteProperties) {
    int returnVal = FALSE;
#ifdef HPUX
    const TCHAR* fix_iconv_hpux;
#endif

    /* The properties being loaded here will not be re-loaded, so enable warnings from here. */
    setLogPropertyWarnings(properties, TRUE);

#ifdef HPUX
    fix_iconv_hpux = getStringProperty(properties, TEXT("wrapper.fix_iconv_hpux"), NULL);
    if (fix_iconv_hpux && (strcmpIgnoreCase(fix_iconv_hpux, TEXT("ALWAYS")) == 0)) {
        /* If Iconv should be fixed, enable it as soon as possible to get the correct conversion when reloading the configuration. */
        toggleIconvHpuxFix(TRUE);
    }
#endif
#ifdef WIN32
    /* For the community edition, always try to display the system errors in English. */
    setLogSysLangId((SUBLANG_ENGLISH_US << 10) + LANG_ENGLISH);

    /* Always read JVM output using the default Windows ANSI code page. */
    wrapperData->jvm_stdout_codepage = GetACP();
#endif
    
    /* Load log file */
    wrapperLoadLoggingProperties(TRUE);
    
    wrapperAddDefaultProperties(properties);

    /* Decide how sequence gaps should be handled before any other properties are loaded. */
    wrapperData->ignoreSequenceGaps = getBooleanProperty(properties, TEXT("wrapper.ignore_sequence_gaps"), FALSE);

    if (loadSecureFileConfiguration()) {
        returnVal = TRUE;
    }

    /* The logging is ready. Print any queued messages generated while loading the localization.
     *  If not done here and a FATAL error occurs on the second load, the queued messages will appear at last and make it confusing. */    
    maintainLogger();

    if (!returnVal) {
        *logLevelOnOverwriteProperties = properties->logLevelOnOverwrite;
        *exitOnOverwriteProperties = properties->exitOnOverwrite;
    }

    if (properties) {
        initialProperties = properties;
        properties = NULL;
    }
    return returnVal;
}

/**
 * Retrieve the original working directory and store it in wrapperData->originalWorkingDir.
 *
 * Return TRUE if there were any problems.
 */
int getOriginalWorkingDir() {
#ifdef WIN32
    int work;
#endif

    if (wrapperData->originalWorkingDir) {
        free(wrapperData->originalWorkingDir);
    }
#ifdef WIN32
    /* Get buffer size, including '\0' */
    work = GetFullPathName(TEXT("."), 0, NULL, NULL);
    if (!work) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL,
            TEXT("Unable to resolve the original working directory: %s"), getLastErrorText());
        return TRUE;
    }
    wrapperData->originalWorkingDir = malloc(sizeof(TCHAR) * work);
    if (!wrapperData->originalWorkingDir) {
        outOfMemory(TEXT("WLCP"), 3);
        return TRUE;
    }
    if (!GetFullPathName(TEXT("."), work, wrapperData->originalWorkingDir, NULL)) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL,
            TEXT("Unable to resolve the original working directory: %s"), getLastErrorText());
        return TRUE;
    }
#else
    /* The solaris implementation of realpath will return a relative path if a relative
     *  path is provided.  We always need an absolute path here.  So build up one and
     *  then use realpath to remove any .. or other relative references. */
    wrapperData->originalWorkingDir = malloc(sizeof(TCHAR) * (PATH_MAX + 1));
    if (!wrapperData->originalWorkingDir) {
        outOfMemory(TEXT("WLCP"), 4);
        return TRUE;
    }
    if (_trealpathN(TEXT("."), wrapperData->originalWorkingDir, PATH_MAX + 1) == NULL) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL,
            TEXT("Unable to resolve the original working directory: %s"), getLastErrorText());
        return TRUE;
    }
#endif
    return FALSE;
}

#ifndef WIN32
/**
 * Try getting getting the Group Name for a Group Id.
 *
 * @param groupId Id of the group.
 *
 * @return The group name of NULL if the group doesn't exist for the given Id.
 */
TCHAR* getGroupNameFromId(gid_t groupId) {
    struct group *gr;
    size_t size;
    TCHAR* groupName = NULL;
    
    if (groupId != -1) {
        gr = getgrgid(groupId);
        if (gr) {
            size = mbstowcs(NULL, gr->gr_name, MBSTOWCS_QUERY_LENGTH);
            if (size > (size_t)0) {
                groupName = malloc(sizeof(TCHAR) * (size + 1));
                if (!groupName) {
                    outOfMemory(TEXT("GGNFI"), 1);
                } else {
                    mbstowcs(groupName, gr->gr_name, size + 1);
                }
            }
        }
    }
    return groupName;
}

/**
 * Prints in a buffer a list of group names given their Ids.
 *
 * @param groups list of group Ids.
 * @param ngroups size of the groups list.
 * @param buffer in which to print the group names.
 * @param bufferSize size of the buffer.
 *
 * @return the buffer.
 */
const TCHAR* getGroupListString(gid_t *groups, int ngroups, TCHAR* buffer, int bufferSize) {
    int len = 0;
    int totLen = 0;
    int i;
    TCHAR* pBuffer = buffer;
    TCHAR* groupName;

    buffer[0] = 0;
    if (groups) {
        for (i = 0; i < ngroups; i++) {
            groupName = getGroupNameFromId(groups[i]);
            if (groupName) {
                if (totLen + _tcslen(groupName) + 10 > bufferSize) {
                    /* Should not happen! (+10: ' (ddddd), ') */
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT("Skip group '%s'. Too long."), groupName);
                } else {
                    _sntprintf(pBuffer, bufferSize - totLen, TEXT("%s (%d)"), groupName, groups[i]);
                    len = _tcslen(pBuffer);
                    totLen += len;
                    pBuffer += len;
                    if (i < ngroups - 1) {
                        _tcsncpy(pBuffer, TEXT(", "), bufferSize - totLen);
                        len = 2;
                        totLen += len;
                        pBuffer += len;
                    }
                }
                free(groupName);
            }
        }
        buffer[bufferSize - 1] = 0;
    }
    return buffer;
}

/**
 * Try getting getting the Group Id for a Group Name.
 *
 * @param groupName Name of the group (Wide char).
 * @param pGroupId Pointer to the Group Id to collect.
 *
 * @return TRUE if there were any problems, FALSE if successful.
 */
int getGroupIdFromName(const TCHAR* groupName, gid_t *pGroupId) {
    struct group *gr;
    char *groupNameMB = NULL;
    size_t size;
    int result = TRUE;
    
    if (groupName && groupName[0] != 0) {
        size = wcstombs(NULL, groupName, 0);
        if (size > (size_t)0) {
            groupNameMB = malloc(size + 1);
            if (!groupNameMB) {
                outOfMemory(TEXT("GGIFN"), 1);
            } else {
                wcstombs(groupNameMB, groupName, size + 1);
                gr = getgrnam(groupNameMB);
                if (gr != NULL) {
                    *pGroupId = gr->gr_gid;
                    result = FALSE;
                }
                free(groupNameMB);
            }
        }
    }
    return result;
}

/**
 * Check if the Wrapper process has the right to assign the given group,
 *  and retrieve a list of group Ids of the Wrapper process.
 *
 * @param groupId Id of the group to check.
 * @param pGroups Pointer to a list that will be malloced and filled with the group Ids of the Wrapper process
 *                (in case the Wrapper is not allowed to assign the group).
 * @param pNgroups Pointer to an integer which be set to the number of group in group list (only set if the list is not NULL).
 *
 * @return TRUE if the Wrapper is not allowed to assign the group or if there is any error, FALSE if all is Ok.
 */
int checkGroupCurrentProcess(gid_t groupId, gid_t **pGroups, int *pNgroups) {
    int size;
    int i, j, k;
    gid_t egid; /* effective group Id */
    int egidInGroups = FALSE; /* will be set to TRUE if the effective group Id is found in *pGroups. */
    
    *pGroups = NULL;
    
    if (geteuid() == 0) {
        /* Root user: valid */
        return FALSE;
    }
    
    egid = getegid();
    if (egid == groupId) {
        /* The group Id is the effective group Id of the Wrapper process: valid */
        return FALSE;
    }
    
    size = getgroups(0, NULL);
    if (size == -1) {
        /* Unable to check. Consider invalid! */
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR,
            TEXT("Failed to retrieve the supplementary group IDs of the current process. %s"), getLastErrorText());
        return TRUE;
    }
    *pGroups = (gid_t *)malloc((size + 1) * sizeof(gid_t));
    if (*pGroups == NULL) {
        /* Unable to check. Consider invalid! */
        outOfMemory(TEXT("CGCP"), 1);
        return TRUE;
    }
    if (size > 0) {
        size = getgroups(size, *pGroups);
        if (size == -1) {
            /* Unable to check. Consider invalid! */
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR,
                TEXT("Failed to retrieve the supplementary group IDs of the current process. %s"), getLastErrorText());
            free(*pGroups);
            *pGroups = NULL;
            return TRUE;
        }
        
        for (i = 0; i < size; i++) {
            if ((*pGroups)[i] == groupId) {
                /* The group Id is among the secondary groups of the Wrapper process: valid */
                free(*pGroups);
                *pGroups = NULL;
                return FALSE;
            }
        }
        
        /* The doc says it is unspecified whether the effective group ID is included in list returned by getgroups().
         *  We need to be careful about the following cases:
         *  1) the effective group may be added as a duplicate of another supplementary group Id in the list.
         *  2) the effective group may be skipped even though it is not listed (we need to add it). */
        for (i = 0; i < size; i++) {
            if ((*pGroups)[i] == egid) {
                /* Set a flag to not add the effective group to the list. */
                egidInGroups = TRUE;
                
                /* Check if there is a duplicate later in the list. */
                for (j = i + 1; j < size; j++) {
                    if ((*pGroups)[j] == egid) {
                        /* Duplicate found, remove it. */
                        for (k = j; k < size - 1; k++) {
                            (*pGroups)[k] = (*pGroups)[k + 1];
                        }
                        size--;
                        /* There should be only one duplicate. */
                        break;
                    }
                }
                break;
            }
        }
    }
    *pNgroups = size;
    if (!egidInGroups) {
        /* Add the effective group Id of the Wrapper process. */
        (*pGroups)[size] = egid;
        (*pNgroups) += 1;
    }
    return TRUE;
}

#define GROUPS_BUFFER_LENGTH    1024

/**
 * Load a Group Id (used to change the group of a file).
 *
 * @param pGroupId Pointer to the Group Id to set.
 * @param propertyName Name of the property from which to load the group.
 * @param defaultValue Default Group Id to use.
 * @param defaultPropertyName Used for logging. The name of the default property,
 *                            or NULL if this is the default property.
 * @param preload TRUE if this is a preload call that should have supressed error output.
 *
 * @return TRUE if there were any problems, FALSE if successful.
 */
int loadGroupProperty(gid_t *pGroupId, const TCHAR* propertyName, gid_t defaultValue, const TCHAR* defaultPropertyName, int preload) {
    const TCHAR* propertyValue;
    TCHAR* groupName;
    gid_t groupId;
    TCHAR *endptr;
    int failed = FALSE;
    gid_t *wrapperGroups = NULL;
    int ngroups = 0;
    TCHAR buffer[GROUPS_BUFFER_LENGTH];
    
    propertyValue = getStringProperty(properties, propertyName, NULL);
    if (!propertyValue || (propertyValue[0] == 0) || (strcmpIgnoreCase(propertyValue, TEXT("-UNCHANGED-")) == 0)) {
        /* Default value. */
        *pGroupId = defaultValue;
    } else {
        errno = 0;
        groupId = (gid_t)_tcstoul(propertyValue, &endptr, 10);
        if ((errno == 0) && (propertyValue != endptr) && (!*endptr)) {
            /* 1) Numerical value: a group ID? */
            /* no error && some digits were read && no additional characters remain */
            if (getgrgid(groupId)) {
                /* This user Id exists */
                *pGroupId = groupId;
            } else {
                if (!preload) {
                    log_printf(WRAPPER_SOURCE_WRAPPER, (wrapperData->groupStrict ? LEVEL_FATAL : properties->logWarningLogLevel),
                        TEXT("Encountered an invalid group Id for configuration property %s=%s."),
                        propertyName, propertyValue);
                }
                *pGroupId = defaultValue;
                failed = TRUE;
            }
        } else {
            /* 2) String: a group name? */
            if (getGroupIdFromName(propertyValue, pGroupId)) {
                /* Failed to retrieve the group Id. */
                if (!preload) {
                    log_printf(WRAPPER_SOURCE_WRAPPER, (wrapperData->groupStrict ? LEVEL_FATAL : properties->logWarningLogLevel),
                        TEXT("Encountered an invalid group name for configuration property %s=%s."),
                        propertyName, propertyValue);
                }
                *pGroupId = defaultValue;
                failed = TRUE;
            }
        }
        if (*pGroupId != defaultValue) {
            /* Check that the Wrapper has the right to change the group. */
            if (checkGroupCurrentProcess(*pGroupId, &wrapperGroups, &ngroups)) {
                if (!preload) {
                    log_printf(WRAPPER_SOURCE_WRAPPER, (wrapperData->groupStrict ? LEVEL_FATAL : properties->logWarningLogLevel),
                        TEXT("The Wrapper process is not allowed to assign the group specified by configuration property %s=%s."),
                        propertyName, propertyValue);
                    if (wrapperData->isAdviserEnabled && wrapperGroups) {
                        log_printf(WRAPPER_SOURCE_WRAPPER, (wrapperData->groupStrict ? LEVEL_FATAL : properties->logWarningLogLevel),
                            TEXT("The Wrapper should either run as root or as a user which belongs to the group you want to assign.\n  The Wrapper process is a member of the following groups:\n  %s"), getGroupListString(wrapperGroups, ngroups, buffer, GROUPS_BUFFER_LENGTH));
                    }
                }
                *pGroupId = defaultValue;
                failed = TRUE;
            }
            free(wrapperGroups);
        }
        if (!preload && failed && !wrapperData->groupStrict) {
            if (defaultPropertyName) {
                groupName = getGroupNameFromId(*pGroupId);
                log_printf(WRAPPER_SOURCE_WRAPPER, properties->logWarningLogLevel,
                    TEXT("  Resolving to the value of %s (%s)."),
                    defaultPropertyName, groupName ? groupName : TEXT("-UNCHANGED-"));
                free(groupName);
            } else {
                log_printf(WRAPPER_SOURCE_WRAPPER, properties->logWarningLogLevel,
                    TEXT("  Resolving to '-UNCHANGED-'."));
            }
        }
    }
    return failed;
}
#endif

static int isServicePasswordNeeded() {
#ifdef WIN32
    return (!strcmpIgnoreCase(wrapperData->argCommand, TEXT("i")) || !strcmpIgnoreCase(wrapperData->argCommand, TEXT("-install")) ||
            !strcmpIgnoreCase(wrapperData->argCommand, TEXT("it")) || !strcmpIgnoreCase(wrapperData->argCommand, TEXT("-installstart")) ||
            !strcmpIgnoreCase(wrapperData->argCommand, TEXT("-customize")));
#else
    return FALSE;
#endif
}

/**
 * This callback is called by configFileReader_Read just after reading a line of the configuration file.
 *  It makes it possible to skip some lines that should be ignored and not stored in memory.
 *
 * @param bufferMB the MB buffer containing the line currently read.
 *
 * Return TRUE if the line should be read, FALSE otherwise.
 */
int confReadFilterCallbackMB(const char *bufferMB) {
    const char* propName = "wrapper.ntservice.password";
    char *ptr;
    
    /* If the service is being installed or the Wrapper being customized, the password property should be used. Otherwise ignore it.
     *  In such modes, the Wrapper executes shortly and the memory will be erased when the property will be disposed. */
    if (!isServicePasswordNeeded()) {
        ptr = strstr(bufferMB, propName);
        if (ptr) {
            ptr += strlen(propName);
            if ((*ptr == '=') || (*ptr == ' ') || (*ptr == '\t')) {
                /* We have exactly "wrapper.ntservice.password" (not "wrapper.ntservice.password.prompt" or any other property) */
#ifdef _DEBUG
                /* NOTE: the address of bufferMB is copied in memory when passed as a parameter to confReadFilterCallback(),
                 *  but the buffer itself is not copied (each character has the same address). */
                printf("confReadFilterCallbackMB - address of bufferMB[0] (%c): %p\n", bufferMB[0], &(bufferMB[0]));
                printf("confReadFilterCallbackMB - address of bufferMB[1] (%c): %p\n", bufferMB[1], &(bufferMB[1]));
#endif
                return FALSE;
            }
        }
    }
    return TRUE;
}

static int checkAndReportInsecureProperties() {
    Property* prop;
    int result = FALSE;
    int logLevel;

    for (prop = properties->first; prop; prop = prop->next) {
        if (prop->hasCipher) {
            if (!prop->allowCiphers) {
                if (prop->isVariable) {
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("The variable definition '%s' contains sensitive data. This is not permitted."), prop->name);
                } else {
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("The value of property '%s' contains sensitive data. This is not permitted."), prop->name);

                    if ((_tcsnicmp(prop->name, TEXT("wrapper.java.additional."), 24) == 0) && _istdigit(prop->name[24]) && (_tcschr(prop->name + 24, TEXT('.')) == NULL)) {
                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("  The %s properties provide a secure alternative to pass system properties to the Java application."), TEXT("wrapper.app.property.<n>"));
                    }
                }
                result = TRUE;
            } else {
                /* Note: Use getBooleanProperty() because wrapper.app.parameter.backend is not be loaded yet. */
                if ((_tcsnicmp(prop->name, TEXT("wrapper.app.parameter."), 22) == 0) && !getBooleanProperty(properties, TEXT("wrapper.app.parameter.backend"), FALSE)) {
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("The value of property '%s' contains sensitive data. This is not permitted."), prop->name);
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("  The %s property can be set to TRUE to securely pass parameters to the Java application."), TEXT("wrapper.app.parameter.backend"));
                    result = TRUE;
                }
            }
        } else if (prop->allowCiphers && isPasswordProperty(prop)) {
            logLevel = getLogLevelForName(getStringProperty(properties, TEXT("wrapper.password_warning.loglevel"), TEXT("WARN")));
            log_printf(WRAPPER_SOURCE_WRAPPER, logLevel, TEXT("The value of property '%s' is not protected. Please enclose it in a cipher notation '%%...|cipher_name%%' to secure it."), prop->name);
        }
    }
    return result;
}

/**
 * This function is used to determine the minimum log level at which messages
 *  should be logged when loading the configuration file.
 *
 * @param preload TRUE if this is a preload pass.
 *
 * @return the log level.
 */
static int getLoadLogLevel(int preload) {
    if (preload) {
        /* Don't show any message during the preload step. */
        return LEVEL_NONE;
    } else if (!wrapperData->argCommandValid) {
        /* The command is invalid. We can still try loading the configuration file to configure the logging settings,
         *  but without reporting any problems as there is no guarantee that the next argument is actually the configuration file. */
        return LEVEL_NONE;
#ifndef WIN32
    } else if (wrapperData->daemonize) {
        /* When running as a daemon, the configuration is loaded 3 times. This flag is set to TRUE after the second load.
         *  To avoid duplicate messages between the second and third calls, only log critical errors that would cause the Wrapper to stop. */
        return LEVEL_ERROR;
#endif
    } else {
        /* Log all messages. */
        return LEVEL_DEBUG;
    }
}

/**
 * Load the configuration.
 *
 * @param preload TRUE if the configuration is being preloaded.
 *
 * Return TRUE if there were any problems.
 */
int wrapperLoadConfigurationProperties(int preload) {
    static int logLevelOnOverwriteProperties = LEVEL_NONE;
    static int exitOnOverwriteProperties = FALSE;
    static int preloadFailed = FALSE;
    int i;
    int firstCall;
#ifdef WIN32
    int work;
    int defaultUMask;
    DWORD error;
#else 
    mode_t defaultUMask;
    gid_t logFileGroup;
    int loadGroupResult = FALSE;
#endif
    const TCHAR* prop;
    int loadResult;
    int hasCipher;

    if (preloadFailed) {
        /* The preload has failed with a FATAL error that will not be reported again on the second load. Exit. */
        return TRUE;
    }

    if (properties) {
        firstCall = FALSE;
        if (properties != initialProperties) {
            disposeProperties(properties);
        }
        properties = NULL;
    } else {
        firstCall = TRUE;
        /* This is the first time, so preserve the working directory. */
        if (getOriginalWorkingDir()) {
            if (preload) {  /* no need to try again as we would fail exactly the same on the second load. */
                preloadFailed = TRUE;
            }
            return TRUE;
        }
        if (wrapperData->configFile) {
            free(wrapperData->configFile);
        }
        /* This is the first time, so preserve the full canonical location of the
         *  configuration file. */
        if (_tcscmp(wrapperData->argConfFile, TEXT("-")) == 0) {
            wrapperData->configFile = NULL;
        } else {
#ifdef WIN32
            work = GetFullPathName(wrapperData->argConfFile, 0, NULL, NULL);
            if (!work) {
                if (!wrapperData->argCommandValid) {
                    /* Report an unrecognized command rather than a problem with the configuration file. */
                } else {
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL,
                        TEXT("Unable to resolve the full path of the configuration file, %s: %s"),
                        wrapperData->argConfFile, getLastErrorText());
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL,
                        TEXT("Current working directory is: %s"), wrapperData->originalWorkingDir);
                }
                if (preload) {  /* no need to try again as we would fail exactly the same on the second load. */
                    preloadFailed = TRUE;
                }
                return TRUE;
            }
            wrapperData->configFile = malloc(sizeof(TCHAR) * work);
            if (!wrapperData->configFile) {
                outOfMemory(TEXT("WLCP"), 1);
                if (preload) {  /* no need to try again as we would fail exactly the same on the second load. */
                    preloadFailed = TRUE;
                }
                return TRUE;
            }
            if (!GetFullPathName(wrapperData->argConfFile, work, wrapperData->configFile, NULL)) {
                if (!wrapperData->argCommandValid) {
                    /* Report an unrecognized command rather than a problem with the configuration file. */
                } else {
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL,
                        TEXT("Unable to resolve the full path of the configuration file, %s: %s"),
                        wrapperData->argConfFile, getLastErrorText());
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT(
                        "Current working directory is: %s"), wrapperData->originalWorkingDir);
                }
                if (preload) {  /* no need to try again as we would fail exactly the same on the second load. */
                    preloadFailed = TRUE;
                }
                return TRUE;
            }

            /* Convert the path to its long form. This is especially needed for the service command line,
             *  as users have reported that in some cases Windows goes through the registry and replaces
             *  existing short paths to their long form (possibly containing spaces) without adding quotes. */
            if (convertToLongPath(&wrapperData->configFile)) {
                error = GetLastError();
                if (error == ERROR_FILE_NOT_FOUND) {
                    /* Will be reported later. */
                } else if (!preload) {
                    /* Report an error, but continue with wrapperData->configFile. */
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT("Failed to convert the path of the configuration file to its long form (0x%x)."), error);
                }
            }
#else
            /* The solaris implementation of realpath will return a relative path if a relative
             *  path is provided.  We always need an absolute path here.  So build up one and
             *  then use realpath to remove any .. or other relative references. */
            wrapperData->configFile = malloc(sizeof(TCHAR) * (PATH_MAX + 1));
            if (!wrapperData->configFile) {
                outOfMemory(TEXT("WLCP"), 2);
                if (preload) {  /* no need to try again as we would fail exactly the same on the second load. */
                    preloadFailed = TRUE;
                }
                return TRUE;
            }
            if (_trealpathN(wrapperData->argConfFile, wrapperData->configFile, PATH_MAX + 1) == NULL) {
                /* Most likely the file does not exist.  The wrapperData->configFile has the first
                 *  file that could not be found.  May not be the config file directly if symbolic
                 *  links are involved. */
                if (wrapperData->argConfFileDefault) {
                    /* The output buffer is likely to contain undefined data.
                     * To be on the safe side and in order to report the error
                     *  below correctly we need to override the data first.*/
                    _sntprintf(wrapperData->configFile, PATH_MAX + 1, TEXT("%s"), wrapperData->argConfFile);
                    /* This was the default config file name.  We know that the working directory
                     *  could be resolved so the problem must be that the default config file does
                     *  not exist.  This problem will be reported later and the wrapperData->configFile
                     *  variable will have the correct full path.
                     * Fall through for now and the user will get a better error later. */
                } else {
                    if (!wrapperData->argCommandValid) {
                        /* Report an unrecognized command rather than a problem with the configuration file. */
                    } else {
                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT(
                            "Unable to open configuration file: %s (%s)\n  Current working directory: %s"),
                            wrapperData->argConfFile, getLastErrorText(), wrapperData->originalWorkingDir);
                    }
                    if (preload) {
                        preloadFailed = TRUE;
                    }
                    return TRUE;
                }
            }
#endif
        }
    }

    /* Create a Properties structure. */
    properties = createProperties(!preload && firstCall, logLevelOnOverwriteProperties, exitOnOverwriteProperties, preload ? SECURITY_LEVEL_TRUST : SECURITY_LEVEL_CHECK);
    logLevelOnOverwriteProperties = LEVEL_NONE;
    exitOnOverwriteProperties = FALSE;
    if (!properties) {
        if (preload) { /* OOM reported */
            preloadFailed = TRUE;
        }
        return TRUE;
    }

    if (initPropertyLoading(properties, preload)) {
        return TRUE;
    }
    
    wrapperAddDefaultProperties(properties);

    /* The argument prior to the argBase will be the configuration file, followed
     *  by 0 or more command line properties.  The command line properties need to be
     *  loaded first, followed by the configuration file. */
    if (!isPromptCall()) {
        for (i = 0; i < wrapperData->argCount; i++) {
            hasCipher = FALSE;
            loadResult = addPropertyPair(properties, wrapperData->argValues[i], TRUE, FALSE, &hasCipher);
            if (!preload) {
                /* Only report errors on the second load, then shutdown. */
                if (loadResult > 0) {
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL,
                        TEXT("The argument '%s' is not a valid property name-value pair."),
                        wrapperData->argValues[i]);
                    return TRUE;
                } else if (hasCipher) {
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL,
                        TEXT("Passing sensitive data on command line is not permitted."));
                    return TRUE;
                }
            }
        }
    }

    if (!wrapperData->configFile) {
        /* This happens when "-" was specified (skip the configuration file). */
        if ((!preload) && !(wrapperData->confFileOptional)) {
            /* On preload, even if we don't have a conf file, we still want to load the embedded configuration
             *  and command line properties as they may affect the logging or creation of the log file. */
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Configuration file is required."));
            return TRUE;
        }
    } else {
        /* Now load the configuration file.
         *  When this happens, the working directory MUST be set to the original working dir. */
        /* Only show log errors when this is not the default configuration file, otherwise the usage will be shown. */
        loadResult = loadProperties(properties, wrapperData->configFile, getLoadLogLevel(preload), wrapperData->originalWorkingDir, !preload && !(wrapperData->argConfFileDefault), confReadFilterCallbackMB);
        if (loadResult != CONFIG_FILE_READER_SUCCESS) {
            if (wrapperData->confFileOptional && wrapperData->argConfFileDefault && (loadResult == CONFIG_FILE_READER_OPEN_FAIL)) {
                /* The wrapper was launched without a config file in the arguments, but it is not required. This is normal not to find it. */
            } else {
                /* If this was a default file name then we don't want to show this as
                 *  an error here.  It will be handled by the caller. */
                /* Debug is not yet available as the config file is not yet loaded. */
                if (!preload) {
                    if (!wrapperData->argConfFileDefault) {
                        if (!wrapperData->argCommandValid) {
                            /* Report an unrecognized command rather than a problem with the configuration file. */
                        } else {
                            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Failed to load configuration."));
                        }
                    }
                    return TRUE;
                }
            }
        } else {
            /* Config file found and read successfully. */
            wrapperData->argConfFileFound = TRUE;
        }
    }

    /* The properties have just been loaded. */
    if (preload) {
        /* Get the error exit code just after the properties have been loaded in case we need to stop before the second load.
         *  We will get it again in loadConfiguration() to allow the property to be reloaded. */
        getConfiguredErrorExitCode(TRUE);
    } else if (properties->overwrittenPropertyCausedExit) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Found duplicated properties."));
        return TRUE; /* will cause the wrapper to exit with error code 1 */
    } else if (checkAndReportInsecureProperties()) {
        return TRUE;
    }

    if (firstCall) {
        /* If the working dir was configured, we need to extract it and preserve its value.
         *  This must be done after the configuration has been completely loaded.
         *  Skip this part when the configuration is reloaded for UNIX daemons (firstCall
         *  == FALSE) as there is no reason wrapper.working.dir would have changed. */

        /* If some environment variable could not be expanded we will fail on preload,
         *  so we want to have proper warning before stopping. */
        setLogPropertyWarnings(properties, preload);

        prop = getStringProperty(properties, TEXT("wrapper.working.dir"), TEXT("."));

        /* Restore property warnings. */
        setLogPropertyWarnings(properties, !preload);

        if (prop && (_tcslen(prop) > 0)) {
            if (wrapperData->workingDir) {
                free(wrapperData->workingDir);
            }
            /* Log any error during preload and stop as anyway the path to the log file and language packs will most likely be wrong (unless they are absolute). */
#ifdef WIN32
            work = GetFullPathName(prop, 0, NULL, NULL);
            if (!work) {
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL,
                    TEXT("Unable to resolve the working directory %s: %s"), prop, getLastErrorText());
                if (preload) {
                    preloadFailed = TRUE;
                }
                return TRUE;
            }
            wrapperData->workingDir = malloc(sizeof(TCHAR) * work);
            if (!wrapperData->workingDir) {
                outOfMemory(TEXT("WLCP"), 5);
                if (preload) {
                    preloadFailed = TRUE;
                }
                return TRUE;
            }
            if (!GetFullPathName(prop, work, wrapperData->workingDir, NULL)) {
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL,
                    TEXT("Unable to resolve the working directory %s: %s"), prop, getLastErrorText());
                if (preload) {
                    preloadFailed = TRUE;
                }
                return TRUE;
            }
#else
            /* The solaris implementation of realpath will return a relative path if a relative
             *  path is provided.  We always need an absolute path here.  So build up one and
             *  then use realpath to remove any .. or other relative references. */
            wrapperData->workingDir = malloc(sizeof(TCHAR) * (PATH_MAX + 1));
            if (!wrapperData->workingDir) {
                outOfMemory(TEXT("WLCP"), 6);
                if (preload) {
                    preloadFailed = TRUE;
                }
                return TRUE;
            }
            if (_trealpathN(prop, wrapperData->workingDir, PATH_MAX + 1) == NULL) {
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL,
                    TEXT("Unable to resolve the working directory %s: %s"), prop, getLastErrorText());
                if (preload) {
                    preloadFailed = TRUE;
                }
                return TRUE;
            }
#endif
        }
    }

    /* Now that the configuration is loaded, we need to update the working directory if the user specified one.
     *  This must be done now so that anything that references the working directory, including the log file
     *  and language pack locations will work correctly. */
    if (wrapperData->workingDir && wrapperSetWorkingDir(wrapperData->workingDir)) {
        if (preload) {
            preloadFailed = TRUE;
        }
        return TRUE;
    }
    
    if (wrapperData->umask == -1) {
    /** Get the umask value for the various files. */
#ifdef WIN32
        defaultUMask = _umask(0);
        _umask(defaultUMask);
#else
        defaultUMask = umask((mode_t)0);
        umask(defaultUMask);
#endif    
        wrapperData->umask = getIntProperty(properties, TEXT("wrapper.umask"), defaultUMask);
    }
    wrapperData->javaUmask = getIntProperty(properties, TEXT("wrapper.java.umask"), wrapperData->umask);
    wrapperData->pidFileUmask = getIntProperty(properties, TEXT("wrapper.pidfile.umask"), wrapperData->umask);
    wrapperData->lockFileUmask = getIntProperty(properties, TEXT("wrapper.lockfile.umask"), wrapperData->umask);
    wrapperData->javaPidFileUmask = getIntProperty(properties, TEXT("wrapper.java.pidfile.umask"), wrapperData->umask);
    wrapperData->javaIdFileUmask = getIntProperty(properties, TEXT("wrapper.java.idfile.umask"), wrapperData->umask);
    wrapperData->statusFileUmask = getIntProperty(properties, TEXT("wrapper.statusfile.umask"), wrapperData->umask);
    wrapperData->javaStatusFileUmask = getIntProperty(properties, TEXT("wrapper.java.statusfile.umask"), wrapperData->umask);
    wrapperData->anchorFileUmask = getIntProperty(properties, TEXT("wrapper.anchorfile.umask"), wrapperData->umask);
    setLogfileUmask(getIntProperty(properties, TEXT("wrapper.logfile.umask"), wrapperData->umask));
#ifndef WIN32
    /** Get the group value for the various files. */
    wrapperData->groupStrict = getBooleanProperty(properties, TEXT("wrapper.group.strict"), FALSE);
    
    /* On a strict mode, make sure to load everything before stopping to get all error messages. */
    loadGroupResult |= loadGroupProperty(&wrapperData->group,               TEXT("wrapper.group"),                 -1,                 NULL, preload); /* -1 means the group is unchanged */
 /* loadGroupResult |= loadGroupProperty(&wrapperData->javaGroup,           TEXT("wrapper.java.group"),            wrapperData->group, TEXT("wrapper.group"), preload); */ /* TODO: can this exist? */
    loadGroupResult |= loadGroupProperty(&wrapperData->pidFileGroup,        TEXT("wrapper.pidfile.group"),         wrapperData->group, TEXT("wrapper.group"), preload);
    loadGroupResult |= loadGroupProperty(&wrapperData->lockFileGroup,       TEXT("wrapper.lockfile.group"),        wrapperData->group, TEXT("wrapper.group"), preload);
    loadGroupResult |= loadGroupProperty(&wrapperData->javaPidFileGroup,    TEXT("wrapper.java.pidfile.group"),    wrapperData->group, TEXT("wrapper.group"), preload);
    loadGroupResult |= loadGroupProperty(&wrapperData->javaIdFileGroup,     TEXT("wrapper.java.idfile.group"),     wrapperData->group, TEXT("wrapper.group"), preload);
    loadGroupResult |= loadGroupProperty(&wrapperData->statusFileGroup,     TEXT("wrapper.statusfile.group"),      wrapperData->group, TEXT("wrapper.group"), preload);
    loadGroupResult |= loadGroupProperty(&wrapperData->javaStatusFileGroup, TEXT("wrapper.java.statusfile.group"), wrapperData->group, TEXT("wrapper.group"), preload);
    loadGroupResult |= loadGroupProperty(&wrapperData->anchorFileGroup,     TEXT("wrapper.anchorfile.group"),      wrapperData->group, TEXT("wrapper.group"), preload);
    loadGroupResult |= loadGroupProperty(&logFileGroup,                     TEXT("wrapper.logfile.group"),         wrapperData->group, TEXT("wrapper.group"), preload);
    setLogfileGroup(logFileGroup);
    
    if (!preload && wrapperData->groupStrict && loadGroupResult) {
        return TRUE;
    }
#endif
    
    if (preload) {
        /* We are only preloading */
        if (firstCall) {
            /* This affects basically language specific variables (not needed when re-loading the configuration). */
            if (wrapperPreLoadConfigurationProperties(&logLevelOnOverwriteProperties, &exitOnOverwriteProperties)) {
                preloadFailed = TRUE;
                return TRUE; /* Actually the return code is ignored on preload. */
            }
        }

        /* If the working dir has been changed then we need to restore it before
         *  the configuration can be reloaded.  This is needed to support relative
         *  references to include files. */
        if (wrapperData->workingDir && wrapperData->originalWorkingDir) {
            if (wrapperSetWorkingDir(wrapperData->originalWorkingDir)) {
                /* Failed to restore the working dir. The configuration can't be reloaded correctly. Shutdown the Wrapper. */
                preloadFailed = TRUE;
                return TRUE; /* Actually the return code is ignored on preload. */
            }
        }
    } else {
        if (firstCall) {
#ifndef WIN32
            wrapperData->daemonize = getBooleanProperty(properties, TEXT("wrapper.daemonize"), FALSE);
            if (wrapperData->daemonize) {
                /** If in the first call here and the wrapper will daemonize, then we don't need to proceed
                 *   any further as the properties will be loaded properly at the second time... */
                return FALSE;
            }
#endif
            /* initialProperties points to the properties loaded during preload. */
            adjustStickyProperties(properties, initialProperties, TRUE);
            disposeProperties(initialProperties);

            initialProperties = properties;
        }

        /* Load the configuration. */
        if ((strcmpIgnoreCase(wrapperData->argCommand, TEXT("-translate")) != 0) && loadConfiguration()) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL,
                TEXT("Problem loading the Wrapper configuration file: %s"), wrapperData->configFile);
            return TRUE;
        }
    }
    return FALSE;
}

static void disposeParameterFile(ParameterFile* parameterFile) {
    if (parameterFile) {
        if (parameterFile->hasCipher) {
            wrapperSecureFreeStringArray(parameterFile->params, parameterFile->paramsCount);
        } else {
            wrapperFreeStringArray(parameterFile->params, parameterFile->paramsCount);
        }
        free(parameterFile->scopes);
        free(parameterFile);
    }
}

int wrapperLoadParameterFileInner(TCHAR **strings, int* scopes, const TCHAR *filePath, const TCHAR *propName, int required, int escapeQuote, int isJVMParameter, int isAppProperty, int preload, int* pHasCipher);

static int wrapperLoadParameterFile(ParameterFile** pParameterFile, const TCHAR *propName, int isJVMParameter, int isAppProperty) {
    const TCHAR *filePath;
    TCHAR prop[256];
    int required;
    int quotable;
    int len;
    TCHAR** strings;
    int* scopes;
    int hasCipher = FALSE;

    /* First clear the old data if they were already loaded. */
    if (*pParameterFile) {
        disposeParameterFile(*pParameterFile);
        *pParameterFile = NULL;
    }

    /* Was the property set? */
    filePath = getFileSafeStringProperty(properties, propName, TEXT(""));
    if (_tcslen(filePath) == 0) {
        return FALSE;
    }

    /* Is the file required? */
    _sntprintf(prop, 256, TEXT("%s.required"), propName);
    required = getBooleanProperty(properties, prop, TRUE);

    /* Allow quotes? (to support several arguments per line) */
    _sntprintf(prop, 256, TEXT("%s.quotable"), propName);
    quotable = getBooleanProperty(properties, prop, FALSE);

    /* Request parameters length. */
    len = wrapperLoadParameterFileInner(NULL, NULL, filePath, propName, required, quotable, isJVMParameter, isAppProperty, TRUE, NULL);

    if (len < 0) {
        /* Failed. Second load to log messages. */
        wrapperLoadParameterFileInner(NULL, NULL, filePath, propName, required, quotable, isJVMParameter, isAppProperty, FALSE, NULL);
        return TRUE;
    } else if (len == 0) {
        /* No parameters. Ignore. */
        log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_DEBUG, TEXT("Parameter file specified by '%s' found, but contains no parameters.  Skipping."), propName);
        return FALSE;
    } else {
        /* Parameters found. Alloc parameters array. */
        strings = (TCHAR**)malloc(sizeof(TCHAR*) * len);
        if (!strings) {
            outOfMemory(TEXT("WLPF"), 1);
            return TRUE;
        }
        memset(strings, 0, sizeof(TCHAR*) * len);

        scopes = (int*)malloc(sizeof(int) * len);
        if (!scopes) {
            outOfMemory(TEXT("WLPF"), 2);
            return TRUE;
        }
        memset(scopes, 0, sizeof(int) * len);

        /* Fill parameters array (if the file contains sensitive data, its permissions will be checked).
         * Notes: - The additional_file should never be allowed to contain sensitive data, but that will be checked later
         *          together with the additional properties. Still verify its permissions to be consistent with other files.
         *        - The parameter_file is only allowed to contain sensitive data if parameters are sent via the backend, but
         *          it is important to always check its permissions because the mode (backend or command line) can change.
         *          We want permissions to be defined correctly during the first execution of the Wrapper. */
        if (wrapperLoadParameterFileInner(strings, scopes, filePath, propName, required, quotable, isJVMParameter, isAppProperty, FALSE, &hasCipher) < 0) {
            goto disposeParams;
        }

        /* Store collected info. */
        *pParameterFile = malloc(sizeof(ParameterFile));
        if (!(*pParameterFile)) {
            outOfMemory(TEXT("WLPF"), 3);
            goto disposeParams;
        }
        (*pParameterFile)->params = strings;
        (*pParameterFile)->scopes = scopes;
        (*pParameterFile)->paramsCount = len;
        (*pParameterFile)->hasCipher = hasCipher;

        return FALSE;

      disposeParams:
        /* Failed */
        if (hasCipher) {
            wrapperSecureFreeStringArray(strings, len);
        } else {
            wrapperFreeStringArray(strings, len);
        }
        return TRUE;
    }
}

static int wrapperLoadParameterFiles() {
    int result = FALSE;

    if (wrapperLoadParameterFile(&wrapperData->parameterFile,  TEXT("wrapper.app.parameter_file"),   FALSE, FALSE)) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Failed to load parameter file '%s'."), TEXT("wrapper.app.parameter_file"));
        result = TRUE;
    }
    if (wrapperLoadParameterFile(&wrapperData->propertyFile,   TEXT("wrapper.app.property_file"),    FALSE, TRUE)) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Failed to load parameter file '%s'."), TEXT("wrapper.app.property_file"));
        result = TRUE;
    }
    if (wrapperLoadParameterFile(&wrapperData->additionalFile, TEXT("wrapper.java.additional_file"), TRUE,  FALSE)) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Failed to load parameter file '%s'."), TEXT("wrapper.java.additional_file"));
        result = TRUE;
    }

    return result;
}

int loadConfigurationSettings(int doPreload) {
    int result = FALSE;

    /* Reset this flag (important if we are reloading the configuration). */
    if (wrapperData->insecure) {
        wrapperData->insecure = FALSE;
    }

    /* Make sure to (re-)create the hashmap of secure files before loading the configuration. */
    if (resetSecureFileChecks()) {
        return TRUE;
    }

    if (doPreload) {
        wrapperLoadConfigurationProperties(TRUE);

#ifdef WIN32
        /* Collect user info that will be used for logging and to resolve the Log On account when running
         *  as a service. This is done after pre-load so that any errors are correctly logged. */
        if (collectUserInfo()) {
            return TRUE; /* For clarity. */
        }
#endif
    }

    if (wrapperLoadConfigurationProperties(FALSE)) {
        if (!wrapperData->argCommandValid) {
            /* Let it fail later to report an unrecognized command rather than a problem with the configuration file. */
            return FALSE;
        } else {
            /* There was a critical error while loading the configuration. For clarity, stop here. */
            return TRUE;
        }
    }

    if (wrapperLoadParameterFiles()) {
        result = TRUE;
    }

    /* Now go through and log any security issues. Stop if critical, but only after all errors have been logged. */
    if (checkAndReportInsecureFiles()) {
        result = TRUE;
    } else if (wrapperData->insecure) {
        /* An invalid cipher was found or a security check failed. */
        result = TRUE;
    }

    return result;
}

void wrapperGetCurrentTime(struct timeb *timeBuffer) {
#ifdef WIN32
    ftime(timeBuffer);
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    timeBuffer->time = (time_t)tv.tv_sec;
    timeBuffer->millitm = (unsigned short)(tv.tv_usec / 1000);
#endif
}

/**
 *  This function stops the pipes (quite in a brutal way)
 */
void protocolStopServerPipe() {
    int closed = FALSE;

    if (protocolActiveServerPipeIn != INVALID_HANDLE_VALUE) {
#ifdef WIN32
        CloseHandle(protocolActiveServerPipeIn);
#else
        close(protocolActiveServerPipeIn);
#endif
        protocolActiveServerPipeIn = INVALID_HANDLE_VALUE;
        closed = TRUE;
    }
    if (protocolActiveServerPipeOut != INVALID_HANDLE_VALUE) {
#ifdef WIN32
        CloseHandle(protocolActiveServerPipeOut);
#else
        close(protocolActiveServerPipeOut);
#endif
        protocolActiveServerPipeOut = INVALID_HANDLE_VALUE;
        closed = TRUE;
    }
    if (closed && wrapperData->isDebugging) {
        log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_DEBUG, TEXT("Backend pipe closed."));
    }
}

/**
 * There is no difference between closing a socket IPv4 vs a socket IPv6
 */
void protocolStopServerSocket() {
    int rc;

    /* Close the socket. */
    if (protocolActiveServerSD != INVALID_SOCKET) {
        if (wrapperData->isDebugging) {
            log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_DEBUG, TEXT("Closing backend server."));
        }
#ifdef WIN32
        rc = closesocket(protocolActiveServerSD);
#else /* UNIX */
        rc = close(protocolActiveServerSD);
#endif
        if (rc == SOCKET_ERROR) {
            if (wrapperData->isDebugging) {
                log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_DEBUG, TEXT("Server socket close failed. (%d)"), wrapperGetSocketLastError());
            }
        }
        protocolActiveServerSD = INVALID_SOCKET;
    }

    wrapperData->actualPort = 0;
}

void protocolStopServer() {
    if (wrapperData->backendTypeBit == WRAPPER_BACKEND_TYPE_PIPE) {
        protocolStopServerPipe();
    } else {
        protocolStopServerSocket();
    }
}

int protocolActiveServerPipeStarted = FALSE;

#ifdef WIN32
HANDLE protocolCreateNamedPipe(TCHAR* pipeName, size_t pipeNameLen, DWORD access) {
    _sntprintf(pipeName, pipeNameLen + 1, TEXT("\\\\.\\pipe\\wrapper-%d-%d-%s"), wrapperData->wrapperPID, wrapperData->jvmRestarts + 1, access == PIPE_ACCESS_INBOUND ? TEXT("in") : TEXT("out"));

    return CreateNamedPipe(pipeName,
                           access,                 /* + FILE_FLAG_FIRST_PIPE_INSTANCE,*/
                           PIPE_TYPE_MESSAGE |     /* message type pipe */
                           PIPE_READMODE_MESSAGE | /* message-read mode */
                           PIPE_NOWAIT,            /* nonblocking mode */
                           1,                      /* only allow 1 connection at a time */
                           32768,
                           32768,
                           0,
                           NULL);
}
#endif

int protocolStartServerPipe() {
#ifdef WIN32
    size_t pipeNameLen;
    TCHAR *pipeName;

    pipeNameLen = 17 + 10 + 1 + 10 + 4;

    pipeName = malloc(sizeof(TCHAR) * (pipeNameLen + 1));
    if (!pipeName) {
        outOfMemory(TEXT("PSSP"), 1);
        return TRUE;
    }

    protocolActiveServerPipeOut = protocolCreateNamedPipe(pipeName, pipeNameLen, PIPE_ACCESS_OUTBOUND);
    if (protocolActiveServerPipeOut == INVALID_HANDLE_VALUE) {
        log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_ERROR, TEXT("Unable to create backend write pipe: %s"), getLastErrorText());
        free(pipeName);
        return TRUE;
    }

    protocolActiveServerPipeIn = protocolCreateNamedPipe(pipeName, pipeNameLen, PIPE_ACCESS_INBOUND);
    if (protocolActiveServerPipeIn == INVALID_HANDLE_VALUE) {
        log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_ERROR, TEXT("Unable to create backend read pipe: %s"), getLastErrorText());
        free(pipeName);
        return TRUE;
    }
    free(pipeName);
#else

    if (pipe(protocolPipeOuFd) == -1) {
        log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_ERROR, TEXT("Unable to create backend write pipe: %s"), getLastErrorText());
        return TRUE;
    }

    if (pipe(protocolPipeInFd) == -1) {
        log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_ERROR, TEXT("Unable to create backend read pipe: %s"), getLastErrorText());
        return TRUE;
    }
#endif

    if (wrapperData->isDebugging) {
        log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_DEBUG, TEXT("Pipe server ready for I/O."));
    }

    protocolActiveServerPipeStarted = TRUE;

    return FALSE;
}

#ifdef WIN32
/**
 * This function doesn't exist on Windows. Similar to inet_ntop that you can find on unix.
 * Convert IPv4 and IPv6 addresses from binary to text form 
 * @param af The family type (AF_INET or AF_INET6)
 * @param src Network address structure
 * @param dst Pointer where to write the text form of the address
 * @return On success, inet_ntop() returns a non-NULL pointer to dst. NULL is returned if there was an error.
 */
const char* inet_ntop(int af, const void* src, char* dst, socklen_t cnt) {
    int result;
    struct sockaddr_in6 addr6;
    struct sockaddr_in  addr4;

    if (af == AF_INET) {
        memset(&addr4, 0, sizeof(struct sockaddr_in));
        memcpy(&(addr4.sin_addr), src, sizeof(addr4.sin_addr));
        addr4.sin_family = af;

        /* here is the function to return a TCHAR. I keep it commented out just as a note */
        /* result = WSAAddressToString((struct sockaddr*) &addr4, sizeof(struct sockaddr_in), 0, dst, (LPDWORD) &cnt); */
        result = getnameinfo((struct sockaddr *)&addr4, sizeof(struct sockaddr_in), dst, cnt, NULL, 0, NI_NUMERICHOST);
    } else {
        memset(&addr6, 0, sizeof(struct sockaddr_in));
        memcpy(&(addr6.sin6_addr), src, sizeof(addr6.sin6_addr));
        addr6.sin6_family = af;

        /* here is the function to return a TCHAR. I keep it commented out just as a note */
        /* result = WSAAddressToString((struct sockaddr*) &addr6, sizeof(struct sockaddr_in6), 0, dst, (LPDWORD) &cnt); */
        result = getnameinfo((struct sockaddr *)&addr6, sizeof(struct sockaddr_in6), dst, cnt, NULL, 0, NI_NUMERICHOST);
    }

    if (result != 0) {
        log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_DEBUG, TEXT("getnameinfo failed (%d): (%s)"), result, getLastErrorText());
        return NULL;
    }
    return dst;
}

/**
 * This function doesn't exist on Windows. Similar to inet_pton that you can find on unix.
 * convert IPv4 and IPv6 addresses from text to binary form
 * @param af The family type (AF_INET or AF_INET6)
 * @param src The adress in text
 * @param dst Pointer where to write the binary form of the address
 * @return FALSE if no error.
 */
int inet_pton(int af, const char *src, void *dst) {
        struct addrinfo hints;
        struct addrinfo *res, *ressave;
        int result;
        int i = 0;
        struct sockaddr_in6 *sockaddr_ipv6;
        struct sockaddr_in  *sockaddr_ipv4;
        
        memset(&hints, 0, sizeof(struct addrinfo));
        hints.ai_family = af;

        result = getaddrinfo(src, NULL, &hints, &res);
        if (result != 0) {
            log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_DEBUG, TEXT("getaddrinfo failed (%d): (%s)"), result, getLastErrorText());
            return TRUE;
        }
        
        ressave = res;

        while (res) {
            switch (res->ai_family) {
                case AF_INET:
                    sockaddr_ipv4 = (struct sockaddr_in *) res->ai_addr;
                    memcpy(dst, &sockaddr_ipv4->sin_addr, sizeof(struct  in_addr));
                    result = FALSE;
                    break;
                case AF_INET6:
                     sockaddr_ipv6 = (struct sockaddr_in6 *) res->ai_addr;
                     memcpy(dst, &sockaddr_ipv6->sin6_addr, sizeof(struct  in6_addr));
                     result = FALSE;
                    break;
                default:
                    log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_DEBUG, TEXT("Ignore unsupported family type: %d"), res->ai_family);
                    break;
            }

            res = res->ai_next;
        }

        freeaddrinfo(ressave);
        return result;
}

#endif

/**
 * Start server using a socket.
 *
 * @param IPv4 if true then we use IPv4, otherwise we use IPv6
 * @return FALSE if socket is created successfully. For some specific error, returns WRAPPER_BACKEND_ERROR_NEXT, for all the other errors returns TRUE
 */
int protocolStartServerSocket(int IPv4) {
    struct sockaddr_in  addr_srv4;
    struct sockaddr_in6 addr_srv6;
    
    int rc;
    int port;
    int fixedPort;
#ifdef UNICODE
    char* tempAddress;
    size_t len;
#endif

    /*int optVal;*/
#ifdef WIN32
    u_long dwNoBlock = TRUE;
#endif

    /* Create the server socket. */
    if (IPv4) {
        protocolActiveServerSD = socket(AF_INET, SOCK_STREAM, 0);
    } else {
        protocolActiveServerSD = socket(AF_INET6, SOCK_STREAM, 0);
    }
    
    if (protocolActiveServerSD == INVALID_SOCKET) {
        log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_ERROR,
            TEXT("Server socket creation failed. (%s)"), getLastErrorText());
        return WRAPPER_BACKEND_ERROR_NEXT;
    }

    /* Make sure the socket is reused. */
    /* We actually do not want to do this as it makes it possible for more than one Wrapper
     *  instance to bind to the same port.  The second instance succeeds to bind, but any
     *  attempts to connect to that port will go to the first Wrapper.  This would of course
     *  cause attempts to launch the second JVM to fail.
     * Leave this code here as a future development note.
    optVal = 1;
#ifdef WIN32
    if (setsockopt(protocolActiveServerSD, SOL_SOCKET, SO_REUSEADDR, (TCHAR *)&optVal, sizeof(optVal)) < 0) {
#else
    if (setsockopt(protocolActiveServerSD, SOL_SOCKET, SO_REUSEADDR, &optVal, sizeof(optVal)) < 0) {
#endif
        log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_ERROR,
            "server socket SO_REUSEADDR failed. (%s)", getLastErrorText());
        wrapperProtocolClose();
        protocolStopServer();
        return;
    }
    */

    /* Make the socket non-blocking */
#ifdef WIN32
    rc = ioctlsocket(protocolActiveServerSD, FIONBIO, &dwNoBlock);
#else /* UNIX  */
    rc = fcntl(protocolActiveServerSD, F_SETFL, O_NONBLOCK);
#endif

    if (rc == SOCKET_ERROR) {
        log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_ERROR,
            TEXT("Server socket ioctlsocket failed. (%s)"), getLastErrorText());
        wrapperProtocolClose();
        protocolStopServer();
        return TRUE;
    }

    /* If a port was specified in the configuration file then we want to
     *  try to use that port or find the next available port.  If 0 was
     *  specified, then we will silently start looking for an available
     *  port starting at 32000. */
    port = wrapperData->port;
    if (port <= 0) {
        port = wrapperData->portMin;
        fixedPort = FALSE;
    } else {
        fixedPort = TRUE;
    }

  tryagain:
    /* Cleanup the socket first */
    if (IPv4) {
        memset(&addr_srv4, 0, sizeof(addr_srv4));
        addr_srv4.sin_family = AF_INET;
        addr_srv4.sin_port = htons((u_short)port);
    } else {
        memset(&addr_srv6, 0, sizeof(addr_srv6));
        addr_srv6.sin6_family = AF_INET6;
        addr_srv6.sin6_flowinfo = 0;
        /* htons switch the 2 bytes. For example:
           32000 in binary: 1111101 00000000
           After swap: 00000000 1111101 which is 125 in decimal */
        addr_srv6.sin6_port = htons((u_short)port);
    }
    
    if (wrapperData->portAddress == NULL) {
        /* the user hasn't defined any address, so we use the loopback address */
        if (IPv4) {
            addr_srv4.sin_addr.s_addr = inet_addr(LOOPBACK_IPv4);
        } else {
            addr_srv6.sin6_addr = LOOPBACK_IPv6;
        }
    } else {
#ifdef UNICODE
#ifdef WIN32
        len = WideCharToMultiByte(CP_OEMCP, 0, wrapperData->portAddress, -1, NULL, 0, NULL, NULL);
        if (len <= 0) {
            log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_WARN,
                TEXT("Invalid multibyte sequence in port address \"%s\" : %s"), wrapperData->portAddress, getLastErrorText());
            return TRUE;
        }
        tempAddress = malloc(len);
        if (!tempAddress) {
            outOfMemory(TEXT("PSSS"), 1);
            return TRUE;
        }
        WideCharToMultiByte(CP_OEMCP, 0, wrapperData->portAddress, -1, tempAddress, (int)len, NULL, NULL);
#else
        len = wcstombs(NULL, wrapperData->portAddress, 0);
        if (len == (size_t)-1) {
            log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_WARN,
                TEXT("Invalid multibyte sequence in port address \"%s\" : %s"), wrapperData->portAddress, getLastErrorText());
            return TRUE;
        }
        tempAddress = malloc(len + 1);
        if (!tempAddress) {
            outOfMemory(TEXT("PSSS"), 2);
            return TRUE;
        }
        wcstombs(tempAddress, wrapperData->portAddress, len + 1);
#endif
        
        /* convert the adress from text to binary form */
        if (IPv4) {
            inet_pton(AF_INET, (const char *)tempAddress, &(addr_srv4.sin_addr));
        } else {
            inet_pton(AF_INET6, (const char *)tempAddress, &(addr_srv6.sin6_addr));
        }

        free(tempAddress);
#else 
        
        if (IPv4) {
            inet_pton(AF_INET, (const char *)wrapperData->portAddress, &(addr_srv4.sin_addr));
        } else {
            inet_pton(AF_INET6, (const char *)wrapperData->portAddress, &(addr_srv6.sin6_addr));
        }

#endif
    }


#ifdef WIN32
    if (IPv4) {
        rc = bind(protocolActiveServerSD, (struct sockaddr FAR *)&addr_srv4, sizeof(addr_srv4));
    } else {
        rc = bind(protocolActiveServerSD, (struct sockaddr FAR *)&addr_srv6, sizeof(addr_srv6));
    }
#else /* UNIX */
    if (IPv4) {
        rc = bind(protocolActiveServerSD, (struct sockaddr *)&addr_srv4, sizeof(addr_srv4));
    } else {
        rc = bind(protocolActiveServerSD, (struct sockaddr *)&addr_srv6, sizeof(addr_srv6));
    }
#endif

    if (rc == SOCKET_ERROR) {
        rc = wrapperGetSocketLastError();

        /* The specified port could not be bound. */
        if ((rc == WRAPPER_EADDRINUSE) || (rc == WRAPPER_EACCES)) {

            /* Address in use, try looking at the next one. */
            if (fixedPort) {
                /* The last port checked was the defined fixed port, switch to the dynamic range. */
                port = wrapperData->portMin;
                fixedPort = FALSE;
                goto tryagain;
            } else {
                port++;
                if (port <= wrapperData->portMax) {
                    goto tryagain;
                }
            }
        }

        /* Log an error.  This is fatal, so die. */
        if (wrapperData->port <= 0) {
            log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_FATAL,
                TEXT("Unable to bind listener to any port in the range %d to %d. (%s)"),
                wrapperData->portMin, wrapperData->portMax, getLastErrorText());
        } else {
            log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_FATAL,
                TEXT("Unable to bind listener port %d, or any port in the range %d to %d. (%s)"),
                wrapperData->port, wrapperData->portMin, wrapperData->portMax, getLastErrorText());
        }

        wrapperStopProcess(getLastError(), TRUE);
        wrapperProtocolClose();
        protocolStopServer();
        wrapperData->exitRequested = TRUE;
        wrapperData->restartRequested = WRAPPER_RESTART_REQUESTED_NO;
        return TRUE;
    }

    /* If we got here, then we are bound to the port */
    if ((wrapperData->port > 0) && (port != wrapperData->port)) {
        log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_INFO, TEXT("Port %d already in use, using port %d instead."), wrapperData->port, port);
    }
    wrapperData->actualPort = port;
    
    if (wrapperData->isDebugging) {
        log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_DEBUG, TEXT("Server listening on port %d."), wrapperData->actualPort);
    }

    /* Tell the socket to start listening. */
    rc = listen(protocolActiveServerSD, 1);
    if (rc == SOCKET_ERROR) {
        log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_ERROR, TEXT("Server socket listen failed. (%d)"), wrapperGetSocketLastError());
        wrapperProtocolClose();
        protocolStopServer();
        return TRUE;
    }

    return FALSE;
}

/**
 * if backendTypeConfiguredBits is 'auto', then it will try in this order:
 *   - socket IPv4
 *   - socket IPv6
 *   - pipe
 * if backendTypeConfiguredBits is 'socket', then it will try in this order:
 *   - socket IPv4
 *   - socket IPv6
 */
void protocolStartServer() {
    int useFallbackAuto = FALSE;
    int useFallbackSocket = FALSE;
    int result;
    
    if (wrapperData->backendTypeConfiguredBits == WRAPPER_BACKEND_TYPE_AUTO) {
        useFallbackAuto = TRUE;
    }

    if (wrapperData->backendTypeConfiguredBits == WRAPPER_BACKEND_TYPE_SOCKET) {
        useFallbackSocket = TRUE;
    }

    if (wrapperData->backendTypeConfiguredBits & WRAPPER_BACKEND_TYPE_SOCKET_V4) {
        result = protocolStartServerSocket(TRUE);
        if (result == WRAPPER_BACKEND_ERROR_NEXT && (useFallbackAuto || useFallbackSocket)) {
            /* we failed to use Ipv4, so lets try with IPv6 */
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Failed to start server using socket IPv4, will try with socket IPv6..."));
        } else if (result == FALSE) {
            /* success */
            wrapperData->backendTypeBit = WRAPPER_BACKEND_TYPE_SOCKET_V4;
            return;
        } else {
            /* error message should have already be printed in protocolStartServerSocket */
            goto error;
        }
    }

    if (wrapperData->backendTypeConfiguredBits & WRAPPER_BACKEND_TYPE_SOCKET_V6) {
        result = protocolStartServerSocket(FALSE);
        if (result == WRAPPER_BACKEND_ERROR_NEXT && useFallbackAuto) {
            /* we failed to use Ipv6, so lets try with pipe */
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Failed to start server socket IPv6, will try with Pipe..."));
        } else if (result == FALSE) {
            /* success */
            wrapperData->backendTypeBit = WRAPPER_BACKEND_TYPE_SOCKET_V6;
            return;
        } else {
            /* error message should have already be printed in protocolStartServerSocket */
            goto error;
        }
    }

    if (wrapperData->backendTypeConfiguredBits & WRAPPER_BACKEND_TYPE_PIPE) {
        result = protocolStartServerPipe();
        if (result == FALSE) {
            /* success */
            wrapperData->backendTypeBit = WRAPPER_BACKEND_TYPE_PIPE;
            return;
        } else {
            /* error message should have already be printed in protocolStartServerPipe */
        }
    }

  error:
    /* if we reach this code, it means we couldn't create a socket or a pipe */
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT("Unable to start server socket."));
    if (!useFallbackAuto) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ADVICE, TEXT("You can set wrapper.backend.type=AUTO, so the wrapper will try to connect to the JVM using ipv4, ipv6 and pipe."));

        if (!useFallbackSocket) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ADVICE, TEXT("You can set wrapper.backend.type=SOCKET, so the wrapper will try to connect to the JVM using ipv4 and ipv6."));
        }
    }
    
    return;
}

/**
 * This function connects the pipes once the other end is there.
 */
void protocolOpenPipe() {
#ifdef WIN32
    int result;
    result = ConnectNamedPipe(protocolActiveServerPipeOut, NULL);

    if (GetLastError() == ERROR_PIPE_LISTENING) {
        return;
    }

    result = ConnectNamedPipe(protocolActiveServerPipeIn, NULL);
    if (GetLastError() == ERROR_PIPE_LISTENING) {
        return;
    }
    if ((result == 0) && (GetLastError() != ERROR_PIPE_CONNECTED) && (GetLastError() != ERROR_NO_DATA)) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Pipe connect failed: %s"), getLastErrorText());
        return;
    }
#else
    /* pipe() automatically opens the file descriptors for read & write operations, we just need to points our variables to the right ends. */
    protocolActiveServerPipeOut = protocolPipeOuFd[1];
    protocolActiveServerPipeIn = protocolPipeInFd[0];
#endif

    protocolActiveServerPipeConnected = TRUE;
}

/**
 * @param IPv4 if true then we use IPv4, otherwise we use IPv6
 */
void protocolOpenSocket(int IPv4) {
    struct sockaddr_in6 addr_srv6;
    struct sockaddr_in  addr_srv4;
    int rc;
    TCHAR* socketSource;
    int req;
#if defined(WIN32)
    u_long dwNoBlock = TRUE;
    u_long addr_srv_len;
#elif defined(HPUX) && !defined(ARCH_IA)
    int addr_srv_len;
#else
    socklen_t addr_srv_len;
#endif
    SOCKET newBackendSD = INVALID_SOCKET;

    char  straddr[256] = {0};
    int port;

    /* Is the server socket open? */
    if (protocolActiveServerSD == INVALID_SOCKET) {
        /* can't do anything yet. */
        return;
    }

    /* Try accepting a socket. */
    if (IPv4) {
        addr_srv_len = sizeof(addr_srv4);
    } else {
        addr_srv_len = sizeof(addr_srv6);
    }

#ifdef WIN32
    if (IPv4) {
        newBackendSD = accept(protocolActiveServerSD, (struct sockaddr FAR *)&addr_srv4, &addr_srv_len);
    } else {
        newBackendSD = accept(protocolActiveServerSD, (struct sockaddr FAR *)&addr_srv6, &addr_srv_len);
    }
#else /* UNIX */
    if (IPv4) {
        newBackendSD = accept(protocolActiveServerSD, (struct sockaddr *)&addr_srv4, &addr_srv_len);
    } else {
        newBackendSD = accept(protocolActiveServerSD, (struct sockaddr *)&addr_srv6, &addr_srv_len);
    }
#endif

    if (newBackendSD == INVALID_SOCKET) {
        rc = wrapperGetSocketLastError();
        /* EWOULDBLOCK != EAGAIN on some platforms. */
        if ((rc == WRAPPER_EWOULDBLOCK) || (rc == EAGAIN)) {
            /* There are no incomming sockets right now. */
            return;
        } else {
            if (wrapperData->isDebugging) {
                log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_DEBUG,
                    TEXT("Socket creation failed. (%s)"), getLastErrorText());
            }
            return;
        }
    }

    /* get a human readable version of the address */
    if (IPv4) {
        inet_ntop(AF_INET, &addr_srv4.sin_addr, straddr, sizeof(straddr));
    } else {
        inet_ntop(AF_INET6, &addr_srv6.sin6_addr, straddr, sizeof(straddr));
    }

    /* convert the address */
#ifdef WIN32
    req = MultiByteToWideChar(CP_OEMCP, 0, straddr, -1, NULL, 0);
    if (req <= 0) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN,
                TEXT("Invalid multibyte sequence in %s: %s"), TEXT("network address"), getLastErrorText());
        return;
    }
    socketSource = malloc(sizeof(TCHAR) * (req + 1));
    if (!socketSource) {
        outOfMemory(TEXT("PO"), 1);
        return;
    }
    MultiByteToWideChar(CP_OEMCP, 0, straddr, -1, socketSource, req + 1);
#else
    req = mbstowcs(NULL, straddr, MBSTOWCS_QUERY_LENGTH);
    if (req == (size_t)-1) {
        log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_WARN,
                TEXT("Invalid multibyte sequence in %s: %s"), TEXT("network address"), getLastErrorText());
        return;
    }
    socketSource = malloc(sizeof(TCHAR) * (req + 1));
    if (!socketSource) {
        outOfMemory(TEXT("PO"), 2);
        return;
    }
    mbstowcs(socketSource, straddr, req + 1);
    socketSource[req] = TEXT('\0'); /* Avoid bufferflows caused by badly encoded characters. */
#endif


    /* Is it already open? */
    if (protocolActiveBackendSD != INVALID_SOCKET) {
        if (IPv4) {
            port = ntohs(addr_srv4.sin_port);
        } else {
            port = ntohs(addr_srv6.sin6_port);
        }

        log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_WARN, TEXT("Ignoring unexpected backend socket connection from %s on port %d"), socketSource, port);
        free(socketSource);
#ifdef WIN32
        rc = closesocket(newBackendSD);
#else /* UNIX */
        rc = close(newBackendSD);
#endif
        if (rc == SOCKET_ERROR) {
            if (wrapperData->isDebugging) {
                log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_DEBUG, TEXT("Socket close failed. (%d)"), wrapperGetSocketLastError());
            }
        }
        return;
    }

    /* New connection, so continue. */
    protocolActiveBackendSD = newBackendSD;

    /* Collect information about the remote end of the socket. */
    if (wrapperData->isDebugging) {
        if (IPv4) {
            port = ntohs(addr_srv4.sin_port);
        } else {
            port = ntohs(addr_srv6.sin6_port);
        }
        log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_DEBUG, TEXT("Accepted a socket on port %d from %s at port %d."), wrapperData->actualPort, socketSource, port);
    }
    
    free(socketSource);
    
    /* Make the socket non-blocking */
#ifdef WIN32
    rc = ioctlsocket(protocolActiveBackendSD, FIONBIO, &dwNoBlock);
#else /* UNIX */
    rc = fcntl(protocolActiveBackendSD, F_SETFL, O_NONBLOCK);
#endif
    if (rc == SOCKET_ERROR) {
        if (wrapperData->isDebugging) {
            log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_DEBUG,
                TEXT("Socket ioctlsocket failed. (%s)"), getLastErrorText());
        }
        wrapperProtocolClose();
        return;
    }
    
    /* We got an incoming connection, so close down the listener to prevent further connections. */
    protocolStopServer();
}

/**
 * Attempt to accept a connection from a JVM client.
 */
void protocolOpen() {
    if (wrapperData->backendTypeBit == WRAPPER_BACKEND_TYPE_PIPE) {
        protocolOpenPipe();
    } else if (wrapperData->backendTypeBit == WRAPPER_BACKEND_TYPE_SOCKET_V6) {
        protocolOpenSocket(FALSE);
    } else {
        protocolOpenSocket(TRUE);
    }
}

void protocolClosePipe() {
    if (protocolActiveServerPipeConnected) {
        if (wrapperData->isDebugging) {
            log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_DEBUG, TEXT("Closing backend pipe."));
        }
#ifdef WIN32
        if ((protocolActiveServerPipeIn != INVALID_HANDLE_VALUE) && !CloseHandle(protocolActiveServerPipeIn)) {
#else
        if (close(protocolActiveServerPipeIn) == -1) {
#endif
            log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_ERROR, TEXT("Failed to close backend pipe: %s"), getLastErrorText());
        }

#ifdef WIN32
        if ((protocolActiveServerPipeOut != INVALID_HANDLE_VALUE) && !CloseHandle(protocolActiveServerPipeOut)) {
#else
        if (close(protocolActiveServerPipeOut) == -1) {
#endif
            log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_ERROR, TEXT("Failed to close backend pipe: %s"), getLastErrorText());
        }

        protocolActiveServerPipeConnected = FALSE;
        protocolActiveServerPipeStarted = FALSE;
        protocolActiveServerPipeIn = INVALID_HANDLE_VALUE;
        protocolActiveServerPipeOut = INVALID_HANDLE_VALUE;
    }
}

void protocolCloseSocket() {
    int rc;

    /* Close the socket. */
    if (protocolActiveBackendSD != INVALID_SOCKET) {
        if (wrapperData->isDebugging) {
            log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_DEBUG, TEXT("Closing backend socket."));
        }
#ifdef WIN32
        rc = closesocket(protocolActiveBackendSD);
#else /* UNIX */
        rc = close(protocolActiveBackendSD);
#endif
        if (rc == SOCKET_ERROR) {
            if (wrapperData->isDebugging) {
                log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_DEBUG, TEXT("Socket close failed. (%d)"), wrapperGetSocketLastError());
            }
        }
        protocolActiveBackendSD = INVALID_SOCKET;
    }
}

/**
 * Close the backend socket.
 */
void wrapperProtocolClose() {
    if (wrapperData->backendTypeBit == WRAPPER_BACKEND_TYPE_PIPE) {
        protocolClosePipe();
    } else {
        protocolCloseSocket();
    }
}

/**
 * Returns the name of a given function code for debug purposes.
 */
TCHAR *wrapperProtocolGetCodeName(char code) {
    static TCHAR unknownBuffer[14];
    TCHAR *name;

    switch (code) {
    case WRAPPER_MSG_PRESTART:
        name = TEXT("PRESTART");
        break;

    case WRAPPER_MSG_START:
        name = TEXT("START");
        break;

    case WRAPPER_MSG_STOP:
        name = TEXT("STOP");
        break;

    case WRAPPER_MSG_RESTART:
        name = TEXT("RESTART");
        break;

    case WRAPPER_MSG_PING:
        name = TEXT("PING");
        break;

    case WRAPPER_MSG_STOP_PENDING:
        name = TEXT("STOP_PENDING");
        break;

    case WRAPPER_MSG_START_PENDING:
        name = TEXT("START_PENDING");
        break;

    case WRAPPER_MSG_STARTED:
        name = TEXT("STARTED");
        break;

    case WRAPPER_MSG_STOPPED:
        name = TEXT("STOPPED");
        break;

    case WRAPPER_MSG_JAVA_PID:
        name = TEXT("JAVA_PID");
        break;

    case WRAPPER_MSG_KEY:
        name = TEXT("KEY");
        break;

    case WRAPPER_MSG_BADKEY:
        name = TEXT("BADKEY");
        break;

    case WRAPPER_MSG_LOW_LOG_LEVEL:
        name = TEXT("LOW_LOG_LEVEL");
        break;

    case WRAPPER_MSG_PING_TIMEOUT: /* No longer used. */
        name = TEXT("PING_TIMEOUT");
        break;

    case WRAPPER_MSG_SERVICE_CONTROL_CODE:
        name = TEXT("SERVICE_CONTROL_CODE");
        break;

    case WRAPPER_MSG_PROPERTIES:
        name = TEXT("PROPERTIES");
        break;

    case WRAPPER_MSG_APP_PROPERTIES:
        name = TEXT("APP_PROPERTIES");
        break;

    case WRAPPER_MSG_APP_PARAMETERS:
        name = TEXT("APP_PARAMETERS");
        break;

    case WRAPPER_MSG_LOG + LEVEL_DEBUG:
        name = TEXT("LOG(DEBUG)");
        break;

    case WRAPPER_MSG_LOG + LEVEL_INFO:
        name = TEXT("LOG(INFO)");
        break;

    case WRAPPER_MSG_LOG + LEVEL_STATUS:
        name = TEXT("LOG(STATUS)");
        break;

    case WRAPPER_MSG_LOG + LEVEL_WARN:
        name = TEXT("LOG(WARN)");
        break;

    case WRAPPER_MSG_LOG + LEVEL_ERROR:
        name = TEXT("LOG(ERROR)");
        break;

    case WRAPPER_MSG_LOG + LEVEL_FATAL:
        name = TEXT("LOG(FATAL)");
        break;

    case WRAPPER_MSG_LOG + LEVEL_ADVICE:
        name = TEXT("LOG(ADVICE)");
        break;

    case WRAPPER_MSG_LOG + LEVEL_NOTICE:
        name = TEXT("LOG(NOTICE)");
        break;

    case WRAPPER_MSG_LOGFILE:
        name = TEXT("LOGFILE");
        break;

    case WRAPPER_MSG_APPEAR_ORPHAN: /* No longer used. */
        name = TEXT("APPEAR_ORPHAN");
        break;

    case WRAPPER_MSG_PAUSE:
        name = TEXT("PAUSE");
        break;

    case WRAPPER_MSG_RESUME:
        name = TEXT("RESUME");
        break;

    case WRAPPER_MSG_GC:
        name = TEXT("GC");
        break;
#ifdef WIN32
    case WRAPPER_MSG_FIRE_CTRL_EVENT:
        name = TEXT("FIRE_CTRL_EVENT");
        break;
#endif
        
    default:
        _sntprintf(unknownBuffer, 14, TEXT("UNKNOWN(%d)"), code);
        name = unknownBuffer;
        break;
    }
    return name;
}

/* Mutex for synchronization of the wrapperProtocolFunction function. */
#ifdef WIN32
HANDLE protocolMutexHandle = NULL;
#else
pthread_mutex_t protocolMutex = PTHREAD_MUTEX_INITIALIZER;
#endif


/** Obtains a lock on the protocol mutex. */
int lockProtocolMutex() {
#ifdef WIN32
    switch (WaitForSingleObject(protocolMutexHandle, INFINITE)) {
    case WAIT_ABANDONED:
        _tprintf(TEXT("Protocol mutex was abandoned.\n"));
        fflush(NULL);
        return -1;
    case WAIT_FAILED:
        _tprintf(TEXT("Protocol mutex wait failed.\n"));
        fflush(NULL);
        return -1;
    case WAIT_TIMEOUT:
        _tprintf(TEXT("Protocol mutex wait timed out.\n"));
        fflush(NULL);
        return -1;
    default:
        /* Ok */
        break;
    }
#else
    if (pthread_mutex_lock(&protocolMutex)) {
        _tprintf(TEXT("Failed to lock the Protocol mutex. %s\n"), getLastErrorText());
        return -1;
    }
#endif

    return 0;
}

/** Releases a lock on the protocol mutex. */
int releaseProtocolMutex() {
#ifdef WIN32
    if (!ReleaseMutex(protocolMutexHandle)) {
        _tprintf(TEXT("Failed to release Protocol mutex. %s\n"), getLastErrorText());
        fflush(NULL);
        return -1;
    }
#else
    if (pthread_mutex_unlock(&protocolMutex)) {
        _tprintf(TEXT("Failed to unlock the Protocol mutex. %s\n"), getLastErrorText());
        return -1;
    }
#endif
    return 0;
}

/**
 * Return TRUE when the JVM's state is between LAUNCHING and STOPPING (and the
 *  stopped packed is not received yet), FALSE otherwise.
 */
int jvmStateExpectsBackendData() {
    if ((wrapperData->jState == WRAPPER_JSTATE_LAUNCHING) ||
        (wrapperData->jState == WRAPPER_JSTATE_LAUNCHED) ||
        (wrapperData->jState == WRAPPER_JSTATE_STARTING) ||
        (wrapperData->jState == WRAPPER_JSTATE_STARTED) ||
        (wrapperData->jState == WRAPPER_JSTATE_STOP) ||
        (wrapperData->jState == WRAPPER_JSTATE_STOPPING)) {

        /* Note: it is important that the stopped state, which is entered in the event of a backend failure, never tries to reopen the connection. */
        if (!wrapperData->stoppedPacketReceived) {
            return TRUE;
        }
    }
    return FALSE;
}

int wrapperGetProtocolState() {
    int result = 0;

    if (jvmStateExpectsBackendData()) {
        result |= WRAPPER_BACKEND_READ_ALLOWED;
    }

    if (((wrapperData->backendTypeBit & WRAPPER_BACKEND_TYPE_SOCKET) && (protocolActiveBackendSD != INVALID_SOCKET)) ||
        ((wrapperData->backendTypeBit == WRAPPER_BACKEND_TYPE_PIPE) && (protocolActiveServerPipeConnected))) {

        result |= WRAPPER_BACKEND_OPENED;

        /* Read is also allowed as long as the backend is opened. */
        result |= WRAPPER_BACKEND_READ_ALLOWED;

        if ((wrapperData->jState == WRAPPER_JSTATE_LAUNCHED) ||
            (wrapperData->jState == WRAPPER_JSTATE_STARTING) ||
            (wrapperData->jState == WRAPPER_JSTATE_STARTED) ||
            (wrapperData->jState == WRAPPER_JSTATE_STOP) ||
            (wrapperData->jState == WRAPPER_JSTATE_STOPPING)) {

            if (!wrapperData->stopPacketReceived) {
                result |= WRAPPER_BACKEND_WRITE_ALLOWED;
            }
        } else if (wrapperData->jState == WRAPPER_JSTATE_LAUNCHING) {
            if (wrapperData->wrongKeyPacketReceived) {
                result |= WRAPPER_BACKEND_WRITE_ALLOWED;
            }
        }
    } else {
        result |= WRAPPER_BACKEND_CLOSED;
    }
    return result;
}

static TCHAR* linearizeStringArray(TCHAR** array, int arrayLen, TCHAR separator, int escape, int allowEmptyValues) {
    int i;
    size_t size;
    TCHAR *c;
    TCHAR *fullBuffer;
    TCHAR *work, *buffer;
    TCHAR strCharSet[3];
    size_t len;

    if (escape) {
        if (allowEmptyValues) {
            /* Empty values will result in two consecutives separators. So separator characters contained in values cannot be escaped with themselves. */
            strCharSet[0] = TEXT('\\');
            strCharSet[1] = separator;
            strCharSet[2] = 0;
        } else {
            /* Separator characters contained in values can be escaped with themselves. */
            strCharSet[0] = separator;
            strCharSet[1] = 0;
        }
    }

    size = 0;
    for (i = 0; i < arrayLen; i++) {
        len = _tcslen(array[i]);
        if (len > 0) {
            /* Add the length of the array element. */
            size += len;

            /* Handle any characters that will need to be escaped. */
            if (escape) {
                c = array[i];
                while ((c = _tcspbrk(c, strCharSet)) != NULL) {
                    size++;
                    c++;
                }
            }
        } else if (!allowEmptyValues) {
            continue;
        }

        size++; /* separator */
    }
    size++; /* null terminated. */

    /* Now that we know how much space this will all take up, allocate a buffer. */
    fullBuffer = buffer = calloc(sizeof(TCHAR), size);
    if (!fullBuffer) {
        return NULL;
    }

    /* Now actually build up the output.  Any separator characters will be escaped with a '\' if allowEmptyValues is TRUE, or with themselves otherwise. */
    for (i = 0; i < arrayLen; i++) {
        len = _tcslen(array[i]);
        if (len > 0) {
            work = array[i];
            if (escape) {
                while ((c = _tcspbrk(work, strCharSet)) != NULL) {
                    _tcsncpy(buffer, work, c - work);
                    buffer += c - work;
                    buffer[0] = allowEmptyValues ? TEXT('\\') : *c;
                    buffer++;
                    buffer[0] = *c;
                    buffer++;
                    work = c + 1;
                }
            }
            _tcsncpy(buffer, work, size - _tcslen(fullBuffer));
            buffer += _tcslen(work);
        } else if (!allowEmptyValues) {
            continue;
        }

        /* separator */
        buffer[0] = separator;
        buffer++;
    }

    /* null terminate. */
    buffer[0] = 0;
    buffer++;

    return fullBuffer;
}

#define PROTOCOL_MAX_WRITE_MS   2000

/**
 * Sends a command to the JVM process.
 *
 * @param function The command to send.  (This is intentionally an 8-bit char.)
 * @param message Message to send along with the command.
 *
 * @return TRUE if there were any problems.
 */
int wrapperProtocolFunction(char function, const TCHAR *messageW) {
    char *protocolSendBuffer = NULL;
#ifdef UNICODE
 #ifdef WIN32
    TCHAR buffer[16];
    UINT cp;
 #endif
#endif
    int err;
    int stop = FALSE;
    int state = 0;
    int rc;
    int cnt;
    int sendCnt;
    int inWritten;
    size_t len;
    TCHAR *temp;
    TCHAR *logMsgW = NULL;
    const TCHAR *messageTemplate;
    char *messageMB = NULL;
    int returnVal = FALSE;
    int ok = TRUE;
    size_t sent;
#ifdef WIN32
    int maxSendSize;
#else
    TCHAR* errorW;
    const char* outputEncoding = MB_UTF8;
#endif

    /* While in most cases sending a signal should be atomic, there are a few cases where it's necessary
     *  to be synchronized so that the sent data don't interleave with writes by other threads:
     *   - On Unix, only writes smaller than PIPE_BUF are guaranteed to be atomic. When sending properties
     *     for example, the buffer can be longer. And we may add other long messages in the future...
     *   - If the pipe/socket would have blocked, the write/send functions will return and the message
     *     will be sent split across multiple calls. */
    if (lockProtocolMutex()) {
        return TRUE;
    }

    if (ok) {
        /* We will be transmitting a MultiByte string of characters.  So we need to convert the messageW. */
        if (messageW) {
#ifdef UNICODE
        /* We handle the backend communication in UTF-8 to allow support of all characters.
         *  This is needed when sending properties to the JVM as the system encoding
         *  may not support certain characters of the configuration file. */
 #ifdef WIN32
            GetLocaleInfo(GetThreadLocale(), LOCALE_IDEFAULTANSICODEPAGE, buffer, sizeof(buffer));
            cp = _ttoi(buffer);
            len = WideCharToMultiByte(CP_UTF8, 0, messageW, -1, NULL, 0, NULL, NULL);
            if (len <= 0) {
                log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_WARN,
                    TEXT("Invalid multibyte sequence in %s \"%s\" : %s"), TEXT("protocol message"), messageW, getLastErrorText());
                returnVal = TRUE;
                ok = FALSE;
            } else {
                messageMB = malloc(len);
                if (!messageMB) {
                    outOfMemory(TEXT("WPF"), 1);
                    returnVal = TRUE;
                    ok = FALSE;
                } else {
                    WideCharToMultiByte(CP_UTF8, 0, messageW, -1, messageMB, (int)len, NULL, NULL);
                }
            }
 #else
            if (converterWideToMB(messageW, &messageMB, outputEncoding) < 0) {
                if (messageMB) {
                    /* An error message is stored in messageMB (we need to convert it to wide chars to display it). */
                    if (converterMBToWide(messageMB, NULL, &errorW, TRUE)) {
                        log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_WARN, TEXT("Unexpected conversion error in %s \"%s\""), TEXT("protocol message"), messageW);
                    } else {
                        log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_WARN, errorW);
                    }
                    if (errorW) {
                        free(errorW);
                    }
                } else {
                    outOfMemory(TEXT("WPF"), 2);
                }
                returnVal = TRUE;
                ok = FALSE;
            }
 #endif
#else
            len = _tscslen(messageW) + 1;
            messageMB = malloc(len);
            if (!messageMB) {
                outOfMemory(TEXT("WPF"), 3);
                returnVal = TRUE;
                ok = FALSE;
            } else {
                _tcsncpy(messageMB, messageW, len);
            }
#endif
        } else {
            messageMB = NULL;
        }
    }

    /* We don't want to show the full properties log message.  It is quite long and distracting. */
    if (function == WRAPPER_MSG_PROPERTIES) {
        messageTemplate = TEXT("(Property Values, Size=%d)");
        len = _tcslen(messageTemplate) + 16 + 1;
        logMsgW = malloc(sizeof(TCHAR) * len);
        if (!logMsgW) {
            outOfMemory(TEXT("WPF"), 4);
            /* Fallback to raw message.  Not ideal but Ok. */
            logMsgW = (TCHAR*)messageW; /* Strip the const, but will never be modified. */
        } else {
            /* messageMB should never be NULL, but for code checker be careful. */
            _sntprintf(logMsgW, len, messageTemplate, (messageMB ? strlen(messageMB) : 0));
        }
    } else if (function == WRAPPER_MSG_APP_PROPERTIES) {
        if (wrapperData->isDebugging) {
            if (appPropertiesLen > 0) {
                temp = linearizeStringArray(appProperties, appPropertiesLen, TEXT(';'), FALSE, FALSE);
                if (!temp) {
                    outOfMemory(TEXT("WPF"), 5);
                } else {
                    if (temp[_tcslen(temp) - 1] == TEXT(';')) {
                        temp[_tcslen(temp) - 1] = 0;
                    }
                    maskSensitiveData(temp, &logMsgW);
                    if (logMsgW) {
                        free(temp);
                    } else {
                        logMsgW = temp;
                    }
                }
            } else {
                updateStringValue(&logMsgW, TEXT(""));
            }
        }
    } else if (function == WRAPPER_MSG_APP_PARAMETERS) {
        if (wrapperData->isDebugging) {
            if (appParametersLen > 0) {
                temp = linearizeStringArray(appParameters, appParametersLen, TEXT(';'), FALSE, TRUE);
                if (!temp) {
                    outOfMemory(TEXT("WPF"), 6);
                } else {
                    if (temp[_tcslen(temp) - 1] == TEXT(';')) {
                        temp[_tcslen(temp) - 1] = 0;
                    }
                    maskSensitiveData(temp, &logMsgW);
                    if (logMsgW) {
                        free(temp);
                    } else {
                        logMsgW = temp;
                    }
                }
            } else {
                updateStringValue(&logMsgW, TEXT(""));
            }
        }
    } else {
        logMsgW = (TCHAR*)messageW; /* Strip the const, but will never be modified. */
    }

    if (wrapperData->stopPacketReceived) {
        log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_DEBUG,
            TEXT("Stopping, not sending packet %s : %s"),
            wrapperProtocolGetCodeName(function), (logMsgW == NULL ? TEXT("NULL") : logMsgW));
        ok = FALSE;
        returnVal = FALSE;
    }

    if (ok) {
        state = wrapperGetProtocolState();
        if (state & WRAPPER_BACKEND_CLOSED) {
            /* Not opened */
            if (wrapperData->isDebugging) {
                log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_STATUS,
                    TEXT("Backend not open, not sending packet %s : %s"),
                    wrapperProtocolGetCodeName(function), (logMsgW == NULL ? TEXT("NULL") : logMsgW));
            }
            returnVal = TRUE;
        } else if (!(state & WRAPPER_BACKEND_WRITE_ALLOWED)) {
            /* Not allowed to write now. */
            if (wrapperData->isDebugging) {
                log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_STATUS,
                    TEXT("Backend not allowing to write (Java state: %s), not sending packet %s : %s"),
                    wrapperGetJState(wrapperData->jState), wrapperProtocolGetCodeName(function), (logMsgW == NULL ? TEXT("NULL") : logMsgW));
            }
            returnVal = TRUE;
        } else {
            /* We need to construct a single string that will be used to transmit the command + message. */
            if (messageMB) {
                len = 1 + strlen(messageMB) + 1;
            } else {
                len = 2;
            }
            protocolSendBuffer = malloc(sizeof(char) * len);
            if (!protocolSendBuffer) {
                outOfMemory(TEXT("WPF"), 7);
                returnVal = TRUE;
                ok = FALSE;
            } else {
                /* Build the packet */
                protocolSendBuffer[0] = function;
                if (messageMB) {
                    strncpy(&(protocolSendBuffer[1]), messageMB, len - 1);
                } else {
                    protocolSendBuffer[1] = 0;
                }
            }

            if (ok) {
                if (wrapperData->isDebugging) {
                    if ((function == WRAPPER_MSG_PING) && messageW && (_tcsstr(messageW, TEXT("silent")) == messageW)) {
                        /*
                        log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_DEBUG,
                            TEXT("Send a silent ping packet %s : %s"),
                            wrapperProtocolGetCodeName(function), (logMsgW == NULL ? TEXT("NULL") : logMsgW));
                        */
                    } else {
                        log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_DEBUG,
                            TEXT("Send a packet %s : %s"),
                            wrapperProtocolGetCodeName(function), (logMsgW == NULL ? TEXT("NULL") : logMsgW));
                    }
                }

                /* When actually sending the packet, we need to be careful to make sure that the entire packet gets sent.
                 *  There isssues on both sockets and Pipes where the send will fail if the packet is too large.
                 *  In such cases, it needs to be broken up into multiple calls.
                 *  This is currently only an issue with the PROPERTIES packet. */
                if (wrapperData->backendTypeBit == WRAPPER_BACKEND_TYPE_PIPE) {
                    sent = 0;
                    cnt = 0;
                    sendCnt = 0;
#ifdef WIN32
                    maxSendSize = 40000;
#endif
                    while ((sent < len) && (cnt < (PROTOCOL_MAX_WRITE_MS / 10))) {
                        if (cnt > 0) {
                            wrapperSleep(10);
                        }

                        /* NOTE: write() will be non-blocking, so it can't be interrupted with EINTR. */
#ifdef WIN32
                        /* Send a maximum of 32000 characters per call as larger values appear to fail without error. */
                        if (WriteFile(protocolActiveServerPipeOut, protocolSendBuffer + sent, __min(maxSendSize, (int)(sizeof(char) * (len - sent))), &inWritten, NULL) == FALSE) {
                            log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_ERROR, TEXT("Writing to the backend pipe failed (%d): %s"), GetLastError(), getLastErrorText());
                            returnVal = TRUE;
                            break;
                        } else if (inWritten == 0) {
                            /* Didn't write anything, but not an error.
                             *  Have not found this documented anywhere, but it happens if the size is larger than some hidden limit.
                             *  EDIT: The doc says: "When writing to a non-blocking, byte-mode pipe handle with insufficient buffer
                             *  space, WriteFile returns TRUE with *lpNumberOfBytesWritten < nNumberOfBytesToWrite." (it is also
                             *  possible that no bytes were written at all) */
                            maxSendSize = __max(512, (int)(maxSendSize * 0.90));
                            sendCnt++;
#else
                        inWritten = write(protocolActiveServerPipeOut, protocolSendBuffer + sent, sizeof(char) * (int)(len - sent));
                        if ((inWritten == -1) && (errno != EAGAIN)) {
                            log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_ERROR, TEXT("Writing to the backend pipe failed (%d): %s"), errno, getLastErrorText());
                            returnVal = TRUE;
                            break;
                        } else if (inWritten == -1) {
                            /* EAGAIN can happen on a non-blocking call if it would have blocked without O_NONBLOCK (but, from what I
                             *  understood, EWOULDBLOCK is only used for sockets, so no need to test it here).
                             *  If len - sent <= PIPE_BUF, write() either succeeds and write all the bytes, or fails and writes nothing.
                             *  It len - sent > PIPE_BUF, a "partial write" may occur but write would return the number of written bytes.
                             *  => in that case inWritten == -1, so no "partial write". */
                            sendCnt++;
#endif
                        } else {
                            /* Write N characters */
                            if (((sent + inWritten < len) || (sendCnt > 0)) && wrapperData->isDebugging) {
                                log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_DEBUG, TEXT("  Sent %d bytes, %d remaining."), inWritten, len - sent - inWritten );
                            }
                            sent += inWritten;
                            sendCnt++;
                        }
                        
                        cnt++;
                    }

                    if (sent < len) {
                        if (sent > 0) {
                            /* An incomplete packet was sent. This will not work on the JVM side, so close the backend and go to the stopped state. */
                            log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_ERROR, TEXT("Send of packet %s was incomplete.  Sent %d of %d bytes.  Backend type: %s"), wrapperProtocolGetCodeName(function), sent, len, TEXT("PIPE"));
                            stop = TRUE;
                        } else if (!returnVal) {
                            /* The error was not reported yet, which means we have just blocked for PROTOCOL_MAX_WRITE_MS without being able to write anything. */
                            log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_ERROR, TEXT("Send of packet %s failed.  Blocked for %d seconds.  Backend type: %s"), wrapperProtocolGetCodeName(function), (int)(PROTOCOL_MAX_WRITE_MS / 1000), TEXT("PIPE"));
                        }
                        returnVal = TRUE;
                    }
                } else {
                    sent = 0;
                    cnt = 0;
                    sendCnt = 0;
                    rc = 0;
                    while ((sent < len) && (cnt < (PROTOCOL_MAX_WRITE_MS / 10))) {
                        if (cnt > 0) {
                            wrapperSleep(10);
                        }

                        /* NOTE: send() will be non-blocking, so it can't be interrupted with EINTR. */
                        rc = send(protocolActiveBackendSD, protocolSendBuffer + sent, sizeof(char) * (int)(len - sent), 0);
                        if (rc == SOCKET_ERROR) {
                            err = wrapperGetSocketLastError();
                            /* EWOULDBLOCK != EAGAIN on some Unix platforms. */
                            if ((err == WRAPPER_EWOULDBLOCK)
#ifndef WIN32
                                || (err == EAGAIN)
#endif
                            ) {
                                /* The output buffer is simply full right now.  Try again in a bit. */
                                sendCnt++;
                            } else {
                                /* On Windows, a common error is WSAECONNRESET (The socket was forcibly closed by the remote host) */
                                log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_ERROR, TEXT("Socket send failed (%d): %s"), err, getErrorText(err, NULL));

                                /* With sockets, the same file descriptor is used to read and write (it is bidirectional).
                                 *  If all goes well, the information written goes to the peer process and cannot be read by the writer. This
                                 *  guarantees that we can only read data from the peer process.
                                 *  But if we fail to write to the socket, it seems to break this bidirectional functionality and will cause
                                 *  to also fail reading information sent by the peer process (in our case the JVM) even if unread data was
                                 *  already there. This has at least been observed on Windows.
                                 *  This JVM will be terminated right below, so reading isn't really necessary anymore anyway.
                                 *  Go ahead and close the connection here to skip the next read attempts. */
                                stop = TRUE;
                                returnVal = TRUE;
                                break;
                            }
                        } else if (rc < 0) {
                            /* According to the doc, this should never happen. */
                            log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_ERROR, TEXT("Send unexpectedly returned %d"), rc);
                            returnVal = TRUE;
                            break;
                        } else {
                            /* Wrote N characters. */
                            if (((sent + rc < len) || (sendCnt > 0)) && wrapperData->isDebugging) {
                                log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_DEBUG, TEXT("  Sent %d bytes, %d remaining."), rc, len - sent - rc );
                            }
                            sent += rc;
                            sendCnt++;
                        }

                        cnt++;
                    }

                    if (sent < len) {
                        if (sent > 0) {
                            /* An incomplete packet was sent. This will not work on the JVM side, so close the backend and go to the stopped state. */
                            log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_ERROR, TEXT("Send of packet %s was incomplete.  Sent %d of %d bytes.  Backend type: %s"), wrapperProtocolGetCodeName(function),  sent, len, TEXT("SOCKET"));
                            stop = TRUE;
                        } else if (!returnVal) {
                            /* The error was not reported yet, which means we have just blocked for PROTOCOL_MAX_WRITE_MS without being able to write anything. */
                            log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_ERROR, TEXT("Send of packet %s failed.  Blocked for %d seconds.  Backend type: %s"), wrapperProtocolGetCodeName(function), (int)(PROTOCOL_MAX_WRITE_MS / 1000), TEXT("SOCKET"));
                        }
                        returnVal = TRUE;
                    }
                }
                if (stop) {
                    if (wrapperData->wrongKeyPacketReceived) {
                        /* This message was not so important. We should keep waiting for the real connection, so stay in this state. */
                    } else {
                        wrapperProtocolClose();
                        log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_STATUS, TEXT("Waiting for the process to complete."));
                        wrapperData->restartRequested = WRAPPER_RESTART_REQUESTED_AUTOMATIC;
                        if (wrapperData->jvmExitTimeout > 0) {
                            wrapperSetJavaState(WRAPPER_JSTATE_STOPPED, wrapperGetTicks(), 5 + wrapperData->jvmExitTimeout);
                        } else {
                            wrapperSetJavaState(WRAPPER_JSTATE_STOPPED, 0, -1);
                        }
                    }
                }
            }
            free(protocolSendBuffer);
        }
    }
    
    /* Free up the logMsgW if we allocated it. */
    if (logMsgW != messageW) {
        free(logMsgW);
    }

    if (messageMB) {
        free(messageMB);
    }

    /* Always make sure the mutex is released. */
    if (releaseProtocolMutex()) {
        returnVal = TRUE;
    }
    return returnVal;
}

/**
 * Checks the status of the server backend.
 *
 * The backend will be initialized if the JVM is in a state where it should
 *  be up, otherwise the backend will be left alone.
 *
 * If the forceOpen flag is set then an attempt will be made to initialize
 *  the backend regardless of the JVM state.
 *
 * Returns TRUE if the server backend is started and ready to accept connections, FALSE if not.
 */
int wrapperCheckServerBackend(int forceOpen) {
    if ((wrapperData->backendTypeBit == 0) ||
        ((wrapperData->backendTypeBit & WRAPPER_BACKEND_TYPE_SOCKET) && (protocolActiveServerSD == INVALID_SOCKET)) ||
        ((wrapperData->backendTypeBit == WRAPPER_BACKEND_TYPE_PIPE) && (protocolActiveServerPipeStarted == FALSE))) {
        /* The backend is not currently open and needs to be started,
         *  unless the JVM is DOWN or in a state where it is not needed. */
        if ((!forceOpen) &&
            ((wrapperData->jState == WRAPPER_JSTATE_DOWN_CLEAN) ||
             (wrapperData->jState == WRAPPER_JSTATE_LAUNCH_DELAY) ||
             (wrapperData->jState == WRAPPER_JSTATE_RESTART) ||
             (wrapperData->jState == WRAPPER_JSTATE_STOPPED) ||
             (wrapperData->jState == WRAPPER_JSTATE_KILLING) ||
             (wrapperData->jState == WRAPPER_JSTATE_KILL) ||
             (wrapperData->jState == WRAPPER_JSTATE_KILLED) ||
             (wrapperData->jState == WRAPPER_JSTATE_DOWN_CHECK) ||
             (wrapperData->jState == WRAPPER_JSTATE_DOWN_FLUSH_STDIN) ||
             (wrapperData->jState == WRAPPER_JSTATE_DOWN_FLUSH))) {
            /* The JVM is down or in a state where the backend is not needed. */
            return FALSE;
        } else {
            /* The backend should be open, try doing so. */
            protocolStartServer();
            if ((wrapperData->backendTypeBit == 0) ||
                ((wrapperData->backendTypeBit & WRAPPER_BACKEND_TYPE_SOCKET) && (protocolActiveServerSD == INVALID_SOCKET)) ||
                ((wrapperData->backendTypeBit == WRAPPER_BACKEND_TYPE_PIPE) && (protocolActiveServerPipeStarted == FALSE))) {
                /* Failed. */
                return FALSE;

            } else {
                return TRUE;
            }
        }
    } else {
        /* Backend is ready. */
        return TRUE;
    }
}

/**
 * Simple function to parse hexidecimal numbers into a TICKS
 */
TICKS hexToTICKS(TCHAR *buffer) {
    TICKS value = 0;
    TCHAR c;
    int pos = 0;
    
    while (TRUE) {
        c = buffer[pos];
        
        if ((c >= TEXT('a')) && (c <= TEXT('f'))) {
            value = (value << 4) + (10 + c - TEXT('a'));
        } else if ((c >= TEXT('A')) && (c <= TEXT('F'))) {
            value = (value << 4) + (10 + c - TEXT('A'));
        } else if ((c >= TEXT('0')) && (c <= TEXT('9'))) {
            value = (value << 4) + (c - TEXT('0'));
        } else {
            /* Any other character or null is the end of the number. */
            return value;
        }
        
        pos++;
    }
}

/**
 * Read any data sent from the JVM.  This function will loop and read as many
 *  packets are available.  The loop will only be allowed to go for 250ms to
 *  ensure that other functions are handled correctly.
 *
 * Returns WRAPPER_PROTOCOLE_READ_COMPLETE if all available data has been read,
 *         WRAPPER_PROTOCOLE_READ_SOCKET_EOF if the stream has performed an orderly shutdown (eof)
 *           Note: This return code is only for socket so that the caller can close the connection.
 *                 For pipe, the writing pipe may remain writable (keep it open to be able to send
 *                 a STOP packet?).
 *         WRAPPER_PROTOCOLE_READ_MORE_DATA if more data is waiting,
 *         WRAPPER_PROTOCOLE_READ_FAILED if reading failed (permanent failure),
 *         WRAPPER_PROTOCOLE_OPEN_FAILED if the connection could not be opened (permanent failure).
 */
int wrapperProtocolRead() {
    char c;
    char code;
    int len;
#ifdef WIN32
    int maxlen;
#endif
    int pos;
    TCHAR *tc;
    int err;
    struct timeb timeBuffer;
    time_t startTime;
    int startTimeMillis;
    time_t now;
    int nowMillis;
    time_t durr;
#ifdef WIN32
    size_t req;
#else
    TCHAR* packetW;
#endif

    if (!(wrapperGetProtocolState() & WRAPPER_BACKEND_READ_ALLOWED)) {
        /* Skip this read. */
        return WRAPPER_PROTOCOLE_READ_COMPLETE;
    }

    wrapperGetCurrentTime(&timeBuffer);
    startTime = now = timeBuffer.time;
    startTimeMillis = nowMillis = timeBuffer.millitm;

    /*
    log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_DEBUG, TEXT("now=%ld, nowMillis=%d"), now, nowMillis);
    */
    while((durr = (now - startTime) * 1000 + (nowMillis - startTimeMillis)) < 250) {
        /*
        log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_DEBUG, TEXT("durr=%ld"), durr);
        */

        if (wrapperGetProtocolState() & WRAPPER_BACKEND_CLOSED) {
            /* A Client backend is not open */
            /* Is the server backend open? */
            if (!wrapperCheckServerBackend(FALSE)) {
                /* Backend is down.  We can not read any packets. */
                return WRAPPER_PROTOCOLE_OPEN_FAILED;
            }

            /* Try accepting a connection */
            protocolOpen();
            if (wrapperGetProtocolState() & WRAPPER_BACKEND_CLOSED) {
                /* JVM did not connect yet. */
                return WRAPPER_PROTOCOLE_READ_COMPLETE;
            }
        }

        if (wrapperData->backendTypeBit & WRAPPER_BACKEND_TYPE_SOCKET) {
            /* Try receiving a packet code */
            
            len = recv(protocolActiveBackendSD, (void*) &c, 1, 0);
            if (len == SOCKET_ERROR) {
                err = wrapperGetSocketLastError();
                /* EWOULDBLOCK != EAGAIN on some Unix platforms. */
                if ((err != WRAPPER_EWOULDBLOCK)
#ifndef WIN32
                    && (err != EAGAIN)
#endif
                ) {
                    if (wrapperData->isDebugging) {
                        log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_DEBUG, TEXT("Socket read failed. %s"), getLastErrorText());
                    }
                    return WRAPPER_PROTOCOLE_READ_FAILED;
                }
                return WRAPPER_PROTOCOLE_READ_COMPLETE;
            } else if (len != 1) {
                if (wrapperData->isDebugging) {
                    log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_DEBUG, TEXT("Socket read no code (eof)."));
                }
                return WRAPPER_PROTOCOLE_READ_SOCKET_EOF;
            }

            code = (char)c;

            /* Read in any message */
            pos = 0;
            do {
                len = recv(protocolActiveBackendSD, (void*) &c, 1, 0);
                if (len == 1) {
                    if (c == 0) {
                        /* End of string */
                        len = 0;
                    } else if (pos < MAX_LOG_SIZE) {
                        packetBufferMB[pos] = c;
                        pos++;
                    }
                } else {
                    len = 0;
                }
            } while (len == 1);

            /* terminate the string; */
            packetBufferMB[pos] = '\0';
        } else if (wrapperData->backendTypeBit == WRAPPER_BACKEND_TYPE_PIPE) {
#ifdef WIN32
            err = PeekNamedPipe(protocolActiveServerPipeIn, NULL, 0, NULL, &maxlen, NULL);
            if ((err == 0) && (GetLastError() == ERROR_BROKEN_PIPE)) {
                /* ERROR_BROKEN_PIPE - the client has closed the pipe. So most likely it just exited */
                protocolActiveServerPipeIn = INVALID_HANDLE_VALUE;
            }
            if (maxlen == 0) {
                /*no data available */
                return WRAPPER_PROTOCOLE_READ_COMPLETE;
            }
            if (ReadFile(protocolActiveServerPipeIn, &c, 1, &len, NULL) || (GetLastError() == ERROR_MORE_DATA)) {
                code = (char)c;
                --maxlen;
                pos = 0;
                do {
                    ReadFile(protocolActiveServerPipeIn, &c, 1, &len, NULL);
                    if (len == 1) {
                        if (c == 0) {
                            /* End of string */
                            len = 0;
                        } else if (pos < MAX_LOG_SIZE) {
                            packetBufferMB[pos] = c;
                            pos++;
                        }
                    } else {
                        len = 0;
                    }
                } while (len == 1 && maxlen-- >= 0);
                packetBufferMB[pos] = '\0';
            } else {
                if (GetLastError() == ERROR_INVALID_HANDLE) {
                    return WRAPPER_PROTOCOLE_READ_COMPLETE;
                } else {
                    if (wrapperData->isDebugging) {
                        log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_DEBUG, TEXT("Pipe read failed. (%s)"), getLastErrorText());
                    }
                    return WRAPPER_PROTOCOLE_READ_FAILED;
                }
            }
#else
            len = read(protocolActiveServerPipeIn, (void*) &c, 1);
            if (len == SOCKET_ERROR) {
                err = errno;
                /* EWOULDBLOCK != EAGAIN on some Unix platforms. */
                if ((err != WRAPPER_EWOULDBLOCK) &&
                    (err != EAGAIN)) {
                    if (wrapperData->isDebugging) {
                        log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_DEBUG, TEXT("Pipe read failed. (%s)"), getLastErrorText());
                    }
                    return WRAPPER_PROTOCOLE_READ_FAILED;
                }
                return WRAPPER_PROTOCOLE_READ_COMPLETE;
            } else if (len == 0) {
                /* nothing read: eof? */
                return WRAPPER_PROTOCOLE_READ_COMPLETE;
            }
            code = (char)c;

            /* Read in any message */
            pos = 0;
            do {
                len = read(protocolActiveServerPipeIn, (void*) &c, 1);
                if (len == 1) {
                    if (c == 0) {
                        /* End of string */
                        len = 0;
                    } else if (pos < MAX_LOG_SIZE) {
                        packetBufferMB[pos] = c;
                        pos++;
                    }
                } else {
                    len = 0;
                }
            } while (len == 1);
            /* terminate the string; */
            packetBufferMB[pos] = '\0';
#endif
        } else {
            /* Should not reach this part because wrapperData->backendTypeBit should always have a valid value */
            return WRAPPER_PROTOCOLE_READ_COMPLETE;
        }

        /* Convert the multi-byte packetBufferMB buffer into a wide-character string. */
        /* Source message is always smaller than the MAX_LOG_SIZE so the output will be as well. */
        /* While the packets sent to the JVM are UTF-8 encoded, it is better to handle the communication
         *  from the JVM to the native Wrapper in the same encoding as stdout (by default the locale encoding). */
#ifdef WIN32
        req = MultiByteToWideChar(getJvmOutputCodePage(), 0, packetBufferMB, -1, packetBufferW, MAX_LOG_SIZE + 1);
        if (req <= 0) {
            log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_WARN,
                    TEXT("Invalid multibyte sequence in %s: %s"), TEXT("protocol message"), getLastErrorText());
            packetBufferW[0] = TEXT('\0');
        }
#else
        if (converterMBToWide(packetBufferMB, getJvmOutputEncodingMB(), &packetW, TRUE)) {
            if (packetW) {
                log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_WARN, packetW);
                free(packetW);
            } else {
                outOfMemory(TEXT("WPR"), 1);
            }
            packetBufferW[0] = TEXT('\0');
        } else {
            _sntprintf(packetBufferW, MAX_LOG_SIZE + 1, TEXT("%s"), packetW);
            packetBufferW[MAX_LOG_SIZE] = TEXT('\0');
            free(packetW);
        }
#endif

        if (wrapperData->isDebugging) {
            if ((code == WRAPPER_MSG_PING) && (_tcsstr(packetBufferW, TEXT("silent")) == packetBufferW)) {
                /*
                log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_DEBUG, TEXT("read a silent ping packet %s : %s"),
                    wrapperProtocolGetCodeName(code), packetBufferW);
                */
            } else {
                log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_DEBUG, TEXT("read a packet %s : %s"),
                    wrapperProtocolGetCodeName(code), packetBufferW);
            }
        }

        switch (code) {
        case WRAPPER_MSG_STOP:
            wrapperStopRequested(_ttoi(packetBufferW));
            break;

        case WRAPPER_MSG_RESTART:
            wrapperRestartRequested();
            break;

        case WRAPPER_MSG_PING:
#ifdef MACOSX
            if (wrapperData->jvmStopped) {
                /* On macOS, we can't use the condition '(sigInfo->si_code == CLD_CONTINUED)' to set
                 *  the wrapperData->signalChildContinuedTrapped flag.  But we can know that the JVM
                 *  is back when we receive a ping.  Then call wrapperCheckAndUpdateProcessStatus()
                 *  to log a message that the JVM was continued. */
                wrapperCheckAndUpdateProcessStatus(wrapperGetTicks(), TRUE);
            }
#endif
            /* Because all versions of the wrapper.jar simply bounce back the ping message, the pingSendTicks should always exist. */
            tc = _tcschr(packetBufferW, TEXT(' '));
            if (tc) {
                /* A pingSendTicks should exist. Parse the id following the space. It will be in the format 0xffffffff. */
                wrapperPingResponded(hexToTICKS(&tc[1]), TRUE);
            } else {
                /* Should not happen, but just in case use the current ticks. */
                wrapperPingResponded(wrapperGetTicks(), FALSE);
            }
            break;

        case WRAPPER_MSG_STOP_PENDING:
            wrapperStopPendingSignaled(_ttoi(packetBufferW));
            break;

        case WRAPPER_MSG_STOPPED:
            wrapperStoppedSignaled();
            break;

        case WRAPPER_MSG_START_PENDING:
            wrapperStartPendingSignaled(_ttoi(packetBufferW));
            break;

        case WRAPPER_MSG_STARTED:
            wrapperStartedSignaled();
            break;

        case WRAPPER_MSG_JAVA_PID:
            wrapperCheckMonitoredProcess(_ttoi(packetBufferW));
            break;

        case WRAPPER_MSG_KEY:
            if (wrapperKeyRegistered(packetBufferW)) {
                return WRAPPER_PROTOCOLE_READ_COMPLETE;
            }
            break;

        case WRAPPER_MSG_LOG + LEVEL_DEBUG:
        case WRAPPER_MSG_LOG + LEVEL_INFO:
        case WRAPPER_MSG_LOG + LEVEL_STATUS:
        case WRAPPER_MSG_LOG + LEVEL_WARN:
        case WRAPPER_MSG_LOG + LEVEL_ERROR:
        case WRAPPER_MSG_LOG + LEVEL_FATAL:
            wrapperLogSignaled(code - WRAPPER_MSG_LOG, packetBufferW);
            break;

        case WRAPPER_MSG_APPEAR_ORPHAN:
            /* No longer used.  This is still here in case a mix of versions are used. */
            break;

        default:
            if (wrapperData->isDebugging) {
                log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_DEBUG, TEXT("received unknown packet (%d:%s)"), code, packetBufferW);
            }
            break;
        }

        /* Get the time again */
        wrapperGetCurrentTime(&timeBuffer);
        now = timeBuffer.time;
        nowMillis = timeBuffer.millitm;
    }
    /*
    log_printf(WRAPPER_SOURCE_PROTOCOL, LEVEL_DEBUG, TEXT("done durr=%ld"), durr);
    */
    if ((durr = (now - startTime) * 1000 + (nowMillis - startTimeMillis)) < 250) {
        return WRAPPER_PROTOCOLE_READ_COMPLETE;
    } else {
        return WRAPPER_PROTOCOLE_READ_MORE_DATA;
    }
}


/******************************************************************************
 * Wrapper inner methods.
 *****************************************************************************/
/**
 * IMPORTANT - Any logging done in here needs to be queued or it would cause a recursion problem.
 *
 * It is also critical that this is NEVER called from within the protocol function because it
 *  would cause a deadlock with the protocol semaphore.  This means that it can never be called
 *  from within log_printf(...).
 */
void wrapperLogFileChanged(const TCHAR *logFile) {
    if (wrapperData->isDebugging) {
        log_printf_queue(TRUE, WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Active log file changed: %s"), logFile);
    }

    /* On startup, this function will always be called the first time the log file is set,
     *  we don't want to send the command in this case as it clutters the debug log output.
     *  Besides, the JVM will not be running anyway. */
    if (wrapperData->jState != WRAPPER_JSTATE_DOWN_CLEAN) {
        wrapperProtocolFunction(WRAPPER_MSG_LOGFILE, logFile);
    }
}

/**
 * Return the size of the PID
 */
int wrapperGetPidSize(int pid, int minSize) {
    if (pid < pow(10, minSize + 1)) {
        return minSize;
    }

    /* Slower method but it should not happen often. */
    return COUNT_DIGITS(pid);
}

/* In the future, we may load this value from configuration properties. */
#define PID_DEFAULT_SIZE    7
#define PID_NA              TEXT("-------") /* Must be the same size as PID_DEFAULT_SIZE */

static int jPidSize = PID_DEFAULT_SIZE;
static int wPidSize = 0;

/**
 * Calculates the size required to display the Wrapper PID column or the Java PID column.
 *  The size of each column can grow on its own as needed and never go back down.
 */
int wrapperLogFormatCount(const TCHAR format, size_t *reqSize) {
    switch( format ) {
    case TEXT('J'):
    case TEXT('j'):
        jPidSize = wrapperGetPidSize((int)wrapperData->javaPID, jPidSize);
        *reqSize += jPidSize + 3;
        return 1;

    case TEXT('W'):
    case TEXT('w'):
        if (wPidSize == 0) {
            /* The Wrapper PID can't change. Calculate its width only one time. */
            wPidSize = wrapperGetPidSize((int)wrapperData->wrapperPID, PID_DEFAULT_SIZE);
        }
        *reqSize += wPidSize + 3;
        return 1;
    }
    
    return 0;
}

/**
 * Print the Wrapper PID column or the Java PID column.
 */
int wrapperLogFormatPrint(const TCHAR format, size_t printSize, TCHAR** pBuffer) {
    switch( format ) {
    case TEXT('J'):
    case TEXT('j'):
        if (wrapperData->javaPID) {
            return _sntprintf(*pBuffer, printSize, TEXT("%*d"), jPidSize, wrapperData->javaPID);
        } else if (wrapperData->javaQueryPID) {
            return _sntprintf(*pBuffer, printSize, TEXT("%*d"), jPidSize, wrapperData->javaQueryPID);
        } else {
            return _sntprintf(*pBuffer, printSize, PID_NA);
        }

    case TEXT('W'):
    case TEXT('w'):
        return _sntprintf(*pBuffer, printSize, TEXT("%*d"), wPidSize, wrapperData->wrapperPID);
    }
    
    return 0;
}

/**
 * Pre initialize the wrapper.
 */
int wrapperInitialize() {
#ifdef WIN32
    int maxPathLen = _MAX_PATH;
#else
    int maxPathLen = PATH_MAX;
#endif

    /* Initialize the properties variable. */
    properties = NULL;

    /* Initialize the random seed. */
    srand((unsigned)time(NULL));

    /* Make sure all values are reliably set to 0. All required values should also be
     *  set below, but this extra step will protect against future changes.  Some
     *  platforms appear to initialize maloc'd memory to 0 while others do not. */
    wrapperData = malloc(sizeof(WrapperConfig));
    if (!wrapperData) {
        _tprintf(TEXT("Out of memory (%s)\n"), TEXT("WIZ1"));
        return 1;
    }
    memset(wrapperData, 0, sizeof(WrapperConfig));
    /* Setup the initial values of required properties. */
    wrapperData->configured = FALSE;
    wrapperData->isConsole = TRUE;
    wrapperSetWrapperState(WRAPPER_WSTATE_STARTING);
    wrapperSetJavaState(WRAPPER_JSTATE_DOWN_CLEAN, 0, -1);
    wrapperData->lastPingTicks = wrapperGetTicks();
    wrapperData->lastLoggedPingTicks = wrapperGetTicks();
    wrapperData->jvmVersionCommand = NULL;
    wrapperData->jvmCommand = NULL;
    wrapperData->jvmDefaultLogLevel = LEVEL_INFO;
    wrapperData->exitRequested = FALSE;
    wrapperData->restartRequested = WRAPPER_RESTART_REQUESTED_INITIAL; /* The first JVM needs to be started. */
    wrapperData->exitCode = 0;
    wrapperData->errorExitCode = 1;
    wrapperData->jvmRestarts = 0;
    wrapperData->jvmLaunchTicks = wrapperGetTicks();
    wrapperData->failedInvocationCount = 0;
    wrapperData->originalWorkingDir = NULL;
    wrapperData->configFile = NULL;
    wrapperData->workingDir = NULL;
    wrapperData->outputFilterCount = 0;
    wrapperData->confDir = NULL;
    wrapperData->umask = -1;
    wrapperData->portAddress = NULL;
    wrapperData->pingTimedOut = FALSE;
    wrapperData->shutdownActionPropertyName = NULL;
    wrapperData->javaVersion = NULL;
    wrapperData->jvmBits = JVM_BITS_UNKNOWN;
    wrapperData->jvmVendor = JVM_VENDOR_UNKNOWN;
#ifdef WIN32
    wrapperData->registry_java_home = NULL;
    if (!(tickMutexHandle = CreateMutex(NULL, FALSE, NULL))) {
        printf("Failed to create tick mutex. %s\n", getLastErrorText());
        return 1;
    }

    /* Initialize control code queue. */
    wrapperData->ctrlCodeQueue = malloc(sizeof(int) * CTRL_CODE_QUEUE_SIZE);
    if (!wrapperData->ctrlCodeQueue) {
        _tprintf(TEXT("Out of memory (%s)\n"), TEXT("WIZ2"));
        return 1;
    }
    wrapperData->ctrlCodeQueueWriteIndex = 0;
    wrapperData->ctrlCodeQueueReadIndex = 0;
    wrapperData->ctrlCodeQueueWrapped = FALSE;
#endif

    if (initLogging(wrapperLogFileChanged)) {
        return 1;
    }

    /* This will only be called by the main thread on startup.
     * Immediately register this thread with the logger.
     * This has to happen after the logging is initialized. */
    logRegisterThread(WRAPPER_THREAD_MAIN);
    
    logRegisterFormatCallbacks(wrapperLogFormatCount, wrapperLogFormatPrint);

    setLogfileFormat(TEXT("LPTM"));
    setLogfileLevelInt(LEVEL_DEBUG);
    setConsoleLogFormat(TEXT("LPM"));
    setConsoleLogLevelInt(LEVEL_DEBUG);
    setSyslogLevelInt(LEVEL_NONE);

    setLogfilePath(TEXT("wrapper.log"), FALSE, FALSE); /* Setting the logfile path may cause some output, so always set the levels and formats first. */
    setLogfileRollMode(ROLL_MODE_SIZE);
    setLogfileAutoClose(FALSE);
    setConsoleFlush(TRUE);  /* Always flush immediately until the logfile is configured to make sure that problems are in a consistent location. */
    setStickyProperties(wrapperStickyPropertyNames);

#ifdef _DEBUG
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("Wrapper Initializing...  Minimum logging configured."));
#endif

    /** Remember what the initial user directory was when the Wrapper was launched. */
    wrapperData->initialPath = (TCHAR *)malloc((maxPathLen + 1) * sizeof(TCHAR));
    if (!wrapperData->initialPath) {
        outOfMemory(TEXT("WIZ"), 3);
        return 1;
    } else {
        if (!(wrapperData->initialPath = _tgetcwd((TCHAR*)wrapperData->initialPath, maxPathLen + 1))) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Failed to get the initial directory. (%s)"), getLastErrorText());
            return 1;
        }
    }
    /* Set a variable to the initial working directory. */
    setEnv(TEXT("WRAPPER_INIT_DIR"), wrapperData->initialPath, ENV_SOURCE_APPLICATION);

#ifdef WIN32
    if (!(protocolMutexHandle = CreateMutex(NULL, FALSE, NULL))) {
        _tprintf(TEXT("Failed to create protocol mutex. %s\n"), getLastErrorText());
        fflush(NULL);
        return 1;
    }
#endif

    /* This is a sanity check to make sure that the datatype used for tick counts is correct. */
    if (sizeof(TICKS) != 4) {
        printf("Tick size incorrect %d != 4\n", (int)sizeof(TICKS));
        fflush(NULL);
        return 1;
    }

    if (loadEnvironment()) {
        return 1;
    }

#ifdef _DEBUG
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("Wrapper Initialization complete."));
#endif
    return 0;
}

void wrapperDataDispose() {
    int i;
    
    if (wrapperData->pingActionList) {
        free(wrapperData->pingActionList);
        wrapperData->pingActionList = NULL;
    }
    if (wrapperData->workingDir) {
        free(wrapperData->workingDir);
        wrapperData->workingDir = NULL;
    }
    if (wrapperData->originalWorkingDir) {
        free(wrapperData->originalWorkingDir);
        wrapperData->originalWorkingDir = NULL;
    }
    if (wrapperData->configFile) {
        free(wrapperData->configFile);
        wrapperData->configFile = NULL;
    }
    if (wrapperData->initialPath) {
        free(wrapperData->initialPath);
        wrapperData->initialPath = NULL;
    }
    if (wrapperData->baseName) {
        free(wrapperData->baseName);
        wrapperData->baseName = NULL;
    }
    if (wrapperData->wrapperJar) {
        free(wrapperData->wrapperJar);
        wrapperData->wrapperJar = NULL;
    }
    if (wrapperData->classpath) {
        free(wrapperData->classpath);
        wrapperData->classpath = NULL;
    }
    if (wrapperData->modulePath) {
        free(wrapperData->modulePath);
        wrapperData->modulePath = NULL;
    }
    if (wrapperData->upgradeModulePath) {
        free(wrapperData->upgradeModulePath);
        wrapperData->upgradeModulePath = NULL;
    }
    if (wrapperData->moduleList) {
        free(wrapperData->moduleList);
        wrapperData->moduleList = NULL;
    }
    if (wrapperData->nativeAccessModuleList) {
        free(wrapperData->nativeAccessModuleList);
        wrapperData->nativeAccessModuleList = NULL;
    }
    if (wrapperData->mainJar) {
        free(wrapperData->mainJar);
        wrapperData->mainJar = NULL;
    }
    if (wrapperData->mainModule) {
        free(wrapperData->mainModule);
        wrapperData->mainModule = NULL;
    }
    if (wrapperData->mainUsrClass) {
        free(wrapperData->mainUsrClass);
        wrapperData->mainUsrClass = NULL;
    }
    if (wrapperData->mainUsrPkg) {
        free(wrapperData->mainUsrPkg);
        wrapperData->mainUsrPkg = NULL;
    }
    if (wrapperData->portAddress) {
        free(wrapperData->portAddress);
        wrapperData->portAddress = NULL;
    }
#ifdef WIN32
    if (wrapperData->userName) {
        free(wrapperData->userName);
        wrapperData->userName = NULL;
    }
    if (wrapperData->domainName) {
        free(wrapperData->domainName);
        wrapperData->domainName = NULL;
    }
    if (wrapperData->ntServiceLoadOrderGroup) {
        free(wrapperData->ntServiceLoadOrderGroup);
        wrapperData->ntServiceLoadOrderGroup = NULL;
    }
    if (wrapperData->ntServiceDependencies) {
        free(wrapperData->ntServiceDependencies);
        wrapperData->ntServiceDependencies = NULL;
    }
    if (wrapperData->ntServiceAccount) {
        free(wrapperData->ntServiceAccount);
        wrapperData->ntServiceAccount = NULL;
    }
    if (wrapperData->ntServicePassword) {
        wrapperSecureFreeStrW(wrapperData->ntServicePassword);
        wrapperData->ntServicePassword = NULL;
    }
    if (wrapperData->ctrlCodeQueue) {
        free(wrapperData->ctrlCodeQueue);
        wrapperData->ctrlCodeQueue = NULL;
    }
#endif
    if (wrapperData->javaQryCmd) {
        free(wrapperData->javaQryCmd);
        wrapperData->javaQryCmd = NULL;
    }
    if (wrapperData->appOnlyAdditionalIndexes) {
        free(wrapperData->appOnlyAdditionalIndexes);
        wrapperData->appOnlyAdditionalIndexes = NULL;
    }
    wrapperDisposeJavaVersionCommand();
    wrapperDisposeJavaBootstrapCommand();
    wrapperDisposeJavaCommand();
    wrapperFreeAppPropertyArray();
    wrapperFreeAppParameterArray();
    if (wrapperData->shutdownActionPropertyName) {
        free(wrapperData->shutdownActionPropertyName);
        wrapperData->shutdownActionPropertyName = NULL;
    }
    if (wrapperData->outputFilterCount > 0) {
        for (i = 0; i < wrapperData->outputFilterCount; i++) {
            if (wrapperData->outputFilters[i]) {
                free(wrapperData->outputFilters[i]);
                wrapperData->outputFilters[i] = NULL;
            }
            if (wrapperData->outputFilterActionLists[i]) {
                free(wrapperData->outputFilterActionLists[i]);
                wrapperData->outputFilterActionLists[i] = NULL;
            }
        }
        if (wrapperData->outputFilters) {
            free(wrapperData->outputFilters);
            wrapperData->outputFilters = NULL;
        }
        if (wrapperData->outputFilterActionLists) {
            free(wrapperData->outputFilterActionLists);
            wrapperData->outputFilterActionLists = NULL;
        }
        if (wrapperData->outputFilterMessages) {
            free(wrapperData->outputFilterMessages);
            wrapperData->outputFilterMessages = NULL;
        }
        if (wrapperData->outputFilterAllowWildFlags) {
            free(wrapperData->outputFilterAllowWildFlags);
            wrapperData->outputFilterAllowWildFlags = NULL;
        }
        if (wrapperData->outputFilterMinLens) {
            free(wrapperData->outputFilterMinLens);
            wrapperData->outputFilterMinLens = NULL;
        }
    }

    if (wrapperData->pidFilename) {
        free(wrapperData->pidFilename);
        wrapperData->pidFilename = NULL;
    }
    if (wrapperData->lockFilename) {
        free(wrapperData->lockFilename);
        wrapperData->lockFilename = NULL;
    }
    if (wrapperData->javaPidFilename) {
        free(wrapperData->javaPidFilename);
        wrapperData->javaPidFilename = NULL;
    }
    if (wrapperData->javaIdFilename) {
        free(wrapperData->javaIdFilename);
        wrapperData->javaIdFilename = NULL;
    }
    if (wrapperData->statusFilename) {
        free(wrapperData->statusFilename);
        wrapperData->statusFilename = NULL;
    }
    if (wrapperData->javaStatusFilename) {
        free(wrapperData->javaStatusFilename);
        wrapperData->javaStatusFilename = NULL;
    }
    if (wrapperData->commandFilename) {
        free(wrapperData->commandFilename);
        wrapperData->commandFilename = NULL;
    }
    if (wrapperData->consoleTitle) {
        free(wrapperData->consoleTitle);
        wrapperData->consoleTitle = NULL;
    }
    if (wrapperData->serviceName) {
        free(wrapperData->serviceName);
        wrapperData->serviceName = NULL;
    }
    if (wrapperData->serviceDisplayName) {
        free(wrapperData->serviceDisplayName);
        wrapperData->serviceDisplayName = NULL;
    }
    if (wrapperData->serviceDescription) {
        free(wrapperData->serviceDescription);
        wrapperData->serviceDescription = NULL;
    }
    if (wrapperData->hostName) {
        free(wrapperData->hostName);
        wrapperData->hostName = NULL;
    }
    if (wrapperData->confDir) {
        free(wrapperData->confDir);
        wrapperData->confDir = NULL;
    }
    if (wrapperData->argConfFileDefault && wrapperData->argConfFile) {
        free(wrapperData->argConfFile);
        wrapperData->argConfFile = NULL;
    }
    disposeJavaVersion(wrapperData->javaVersionMin);
    disposeJavaVersion(wrapperData->javaVersionMax);
    disposeJavaVersion(wrapperData->javaVersion);
#ifdef WIN32
    if (wrapperData->registry_java_home) {
        free(wrapperData->registry_java_home);
        wrapperData->registry_java_home = NULL;
    }
#endif

    if (wrapperData) {
        free(wrapperData);
        wrapperData = NULL;
    }
}

/** Common wrapper cleanup code. */
void wrapperDispose(int exitCode) {
    /* Make sure not to dispose twice.  This should not happen, but check for safety. */
    if (disposed) {
        /* Don't use log_printf here as the second call may have already disposed logging. */
        _tprintf(TEXT("wrapperDispose was called more than once.\n"));
        return;
    }
    disposed = TRUE;

#ifdef WIN32
    if (protocolMutexHandle) {
        if (!CloseHandle(protocolMutexHandle)) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Unable to close protocol mutex handle. %s"), getLastErrorText());
        }
    }
    
    /* Make sure that the startup thread has completed. */
    disposeStartup();
    
    disposeSystemPath();
#endif

    disposeHashMapJvmEncoding();

#ifndef WIN32
    /* Clean up the javaIN thread. */
    disposeJavaIN();
#endif
    
    /* Clean up the javaIO thread. This should be done before the timer thread. */
    if (wrapperData->useJavaIOThread) {
        disposeJavaIO();
    }

    /* Clean up the timer thread. */
    if (!wrapperData->useSystemTime) {
        disposeTimer();
    }

    /* Clean up hashmap of quotable properties. */
    disposeQuotableMap();

    /* Clean up the properties structure. */
    if (initialProperties != properties) {
        disposeProperties(initialProperties);
        initialProperties = NULL;
    }
    disposeProperties(properties);
    properties = NULL;

    /* Clean up the ParameterFile structures. */
    disposeParameterFile(wrapperData->parameterFile);
    wrapperData->parameterFile = NULL;
    disposeParameterFile(wrapperData->propertyFile);
    wrapperData->propertyFile = NULL;
    disposeParameterFile(wrapperData->additionalFile);
    wrapperData->additionalFile = NULL;

    disposeEnvironment();

    disposeSecureFiles();

    if (wrapperChildWorkBuffer) {
        free(wrapperChildWorkBuffer);
        wrapperChildWorkBuffer = NULL;
    }

    /* Note: It is important that all other threads completed at that point, as we are going to dispose the logging. */
    
    /* We will dispose the logging, so wrapperSleep should not be allowed to log anymore. */
    wrapperData->isSleepOutputEnabled = FALSE;

    /* Always call maintain logger once to make sure that all queued messages are logged before we exit. */
    maintainLogger();

    if (wrapperData->runCommonStarted) {
        /* Log the exit code to help with debugging. */
        if (wrapperData->isDebugging) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Exit code: %d"), exitCode);
        }

        /* This will be the last message. */
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("<-- Wrapper Stopped"));
    }

    /* Stop handling signals (the handler is using wrapperData and the logging and would crash if they are already disposed). 
     *  Note: Once this flag is set, CTRL+C will be handled by the parent session and interrupt the process immediately (when
     *        running as a console).  We maintained the logger before to make sure that all output can be printed. */
    handleSignals = FALSE;

    /* Clean up the logging system.  Should happen near last. */
    disposeLogging();

    /* clean up the main wrapper data structure. This must be done last.*/
    wrapperDataDispose();
}

/**
 * Returns the file name base as a newly malloced TCHAR *.  The resulting
 *  base file name will have any path and extension stripped.
 *  If the 'os-arch-bit' pattern is found, it will also be stripped.
 *
 * @param fileName file name or path.
 * @param pBaseName pointer to output buffer. Should be long enough to always
 *        contain the base name (_tcslen(fileName) + 1) is safe.
 */
void wrapperGetFileBase(const TCHAR *fileName, TCHAR **pBaseName) {
    const TCHAR *start;
    const TCHAR *end;
    const TCHAR *c;
    TCHAR buffer[32];

    if ((c = _tcsrchr(fileName, FILE_SEPARATOR_C)) != NULL) {
        /* Strip off any path. */
        start = c + 1;
    } else {
        start = fileName;
    }

    _sntprintf(buffer, 32, TEXT("-%s-%s-%s"), wrapperOS, wrapperArch, wrapperBits);
    buffer[31] = 0;
    if ((c = _tcsstr(start, buffer)) != NULL) {
        /* Strip off '-os-arch-bit'. */
        end = c;
    } else if ((c = _tcsrchr(start, TEXT('.'))) != NULL) {
        /* Strip off any extension. */
        end = c;
    } else {
        end = &start[_tcslen(start)];
    }

    /* Now create the new base name. */
    _tcsncpy(*pBaseName, start, end - start);
    (*pBaseName)[end - start] = TEXT('\0');
}

/**
 * Returns a buffer containing a multi-line version banner.  It is the responsibility of the caller
 *  to make sure it gets freed.
 */
TCHAR *generateVersionBanner() {
    TCHAR *banner = TEXT("Java Service Wrapper %s Edition %s-bit %s\n  Copyright (C) 1999-%s Tanuki Software, Ltd. All Rights Reserved.\n    https://wrapper.tanukisoftware.com%s");
    TCHAR *product = TEXT("Community");
    TCHAR *copyright = TEXT("2025");
    TCHAR *notForProductionWarning = TEXT("");
    TCHAR *buffer;
    size_t len;

    if (_tcschr(wrapperVersionRoot, TEXT('-'))) {
        notForProductionWarning = TEXT("\n\nTHIS IS NOT A STABLE RELEASE, DO NOT USE IN PRODUCTION!\n  Download a stable release from https://wrapper.tanukisoftware.com/doc/english/download.jsp");
    }

    len = _tcslen(banner) + _tcslen(product) + _tcslen(wrapperBits) + _tcslen(wrapperVersionRoot) + _tcslen(copyright) + _tcslen(notForProductionWarning) + 1;
    buffer = malloc(sizeof(TCHAR) * len);
    if (!buffer) {
        outOfMemory(TEXT("GVB"), 1);
        return NULL;
    }

    _sntprintf(buffer, len, banner, product, wrapperBits, wrapperVersionRoot, copyright, notForProductionWarning);

    return buffer;
}

/**
 * Output the version.
 */
void wrapperVersionBanner() {
    TCHAR *banner = generateVersionBanner();
    if (!banner) {
        return;
    }
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, banner);
    free(banner);
}

/**
 * Output the application usage.
 */
void wrapperUsage(TCHAR *appName) {
    TCHAR *confFileBase;

    confFileBase = malloc(sizeof(TCHAR) * (_tcslen(appName) + 1));
    if (!confFileBase) {
        outOfMemory(TEXT("WU"), 1);
        return;
    }
    wrapperGetFileBase(appName, &confFileBase);

    setSimpleLogLevels();

    wrapperVersionBanner();
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT(""));
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("Usage:"));
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("  %s <command> <configuration file> [configuration properties] [...]"), appName);
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("  %s <configuration file> [configuration properties] [...]"), appName);
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("     (<command> implicitly '-c')"));
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("  %s <command>"), appName);
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("     (<configuration file> implicitly '%s.conf')"), confFileBase);
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("  %s"), appName);
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("     (<command> implicitly '-c' and <configuration file> '%s.conf')"), confFileBase);
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT(""));
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("where <command> can be one of:"));
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("  -c  --console run as a Console application"));
#ifdef WIN32
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("  -su --setup   SetUp the Wrapper"));
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("  -td --teardown TearDown the Wrapper"));
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("  -t  --start   starT an NT service"));
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("  -a  --pause   pAuse a running NT service"));
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("  -e  --resume  rEsume a paused NT service"));
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("  -p  --stop    stoP a started NT service"));
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("  -i  --install Install as an NT service"));
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("  -it --installstart Install and sTart as an NT service"));
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("  -r  --remove  Uninstall/Remove as an NT service"));
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("  -l=<code> --controlcode=<code> send a user controL Code to a running NT service"));
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("  -d  --dump    request a thread Dump"));
    /** Return mask: installed:1 running:2 interactive:4 automatic:8 manual:16 disabled:32 */
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("  -q[=serviceName]  --query[=serviceName] Query the current status of the service"));
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("  -qs[=serviceName] --querysilent[=serviceName] Silently Query the current status of the service"));
    /* Omit '-s' option from help as it is only used by the service manager. */
    /*log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("  -s  --service used by service manager")); */
#endif
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("  -v  --version print the Wrapper's Version information"));
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("  -?  --help    print this help message"));
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("  -- <args>     mark the end of Wrapper arguments.  All arguments after the\n                '--' will be passed through unmodified to the java application."));
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT(""));
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("<configuration file> is the conf file to use.  Filename must be absolute or"));
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("  relative to the location of %s"), appName);
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT(""));
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("[configuration properties] are configuration name-value pairs which override values"));
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("  in the Wrapper configuration file.  For example:"));
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("  wrapper.debug=true"));
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT(""));
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("  Please note that any file references must be absolute or relative to the location\n  of the Wrapper executable."));
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT(""));

    free(confFileBase);
}

int wrapperSetDefaultConfFile(const TCHAR *defaultConfFile) {
    size_t len;
    
    if (defaultConfFile) {
        len = _tcslen(defaultConfFile) + 1;
        wrapperData->argConfFile = malloc(len * sizeof(TCHAR));
        if (!wrapperData->argConfFile) {
            outOfMemory(TEXT("WSDCF"), 1);
            return FALSE;
        }
        _tcsncpy(wrapperData->argConfFile, defaultConfFile, len);
    } else {
        len = _tcslen(wrapperData->baseName) + 5 + 1;
        wrapperData->argConfFile = malloc(len * sizeof(TCHAR));
        if (!wrapperData->argConfFile) {
            outOfMemory(TEXT("WSDCF"), 2);
            return FALSE;
        }
        _sntprintf(wrapperData->argConfFile, len, TEXT("%s.conf"), wrapperData->baseName);
    }
    return TRUE;
}

static int isLaunchCommandValid(const TCHAR* cmd) {
    if (!strcmpIgnoreCase(cmd, TEXT("c"))  || !strcmpIgnoreCase(cmd, TEXT("-console")) ||
        !strcmpIgnoreCase(cmd, TEXT("?"))  || !strcmpIgnoreCase(cmd, TEXT("-help")) ||
        !strcmpIgnoreCase(cmd, TEXT("v"))  || !strcmpIgnoreCase(cmd, TEXT("-version")) ||
        !strcmpIgnoreCase(cmd, TEXT("h"))  || !strcmpIgnoreCase(cmd, TEXT("-hostid")) ||
        !strcmpIgnoreCase(cmd, TEXT("-jvm_bits")) ||
        !strcmpIgnoreCase(cmd, TEXT("-request_delta_binary_bits"))) {
        return TRUE;
    }
#ifdef WIN32
    if (!strcmpIgnoreCase(cmd, TEXT("s"))  || !strcmpIgnoreCase(cmd, TEXT("-service")) ||
        !strcmpIgnoreCase(cmd, TEXT("su")) || !strcmpIgnoreCase(cmd, TEXT("-setup")) ||
        !strcmpIgnoreCase(cmd, TEXT("td")) || !strcmpIgnoreCase(cmd, TEXT("-teardown")) ||
        !strcmpIgnoreCase(cmd, TEXT("i"))  || !strcmpIgnoreCase(cmd, TEXT("-install")) ||
        !strcmpIgnoreCase(cmd, TEXT("it")) || !strcmpIgnoreCase(cmd, TEXT("-installstart")) ||
        !strcmpIgnoreCase(cmd, TEXT("r"))  || !strcmpIgnoreCase(cmd, TEXT("-remove")) ||
        !strcmpIgnoreCase(cmd, TEXT("t"))  || !strcmpIgnoreCase(cmd, TEXT("-start")) ||
        !strcmpIgnoreCase(cmd, TEXT("a"))  || !strcmpIgnoreCase(cmd, TEXT("-pause")) ||
        !strcmpIgnoreCase(cmd, TEXT("e"))  || !strcmpIgnoreCase(cmd, TEXT("-resume")) ||
        !strcmpIgnoreCase(cmd, TEXT("p"))  || !strcmpIgnoreCase(cmd, TEXT("-stop")) ||
        !strcmpIgnoreCase(cmd, TEXT("l"))  || !strcmpIgnoreCase(cmd, TEXT("-controlcode")) ||
        !strcmpIgnoreCase(cmd, TEXT("q"))  || !strcmpIgnoreCase(cmd, TEXT("-query")) ||
        !strcmpIgnoreCase(cmd, TEXT("qs")) || !strcmpIgnoreCase(cmd, TEXT("-querysilent")) ||
        !strcmpIgnoreCase(cmd, TEXT("d"))  || !strcmpIgnoreCase(cmd, TEXT("-dump"))) {
        return TRUE;
    }
#else
    if (!strcmpIgnoreCase(cmd, TEXT("-translate")) ||
        !strcmpIgnoreCase(cmd, TEXT("-request_log_file")) ||
        !strcmpIgnoreCase(cmd, TEXT("-request_default_log_file"))) {
        return TRUE;
    }
#endif
    return FALSE;
}

/**
 * Parse the main arguments.
 *
 * Returns FALSE if the application should exit with an error.  A message will
 *  already have been logged.
 */
int wrapperParseArguments(int argc, TCHAR **argv) {
    TCHAR *c;
    int delimiter, wrapperArgCount;
    TCHAR *defaultConfFile = NULL;
    wrapperData->javaArgValueCount = 0;
    delimiter = 1;

    if (argc > 1
        ) {
        for (delimiter = 0; delimiter < argc ; delimiter++) {
            if ( _tcscmp(argv[delimiter], TEXT("--")) == 0) {
#if !defined(WIN32) && defined(UNICODE)
                free(argv[delimiter]);
#endif
                argv[delimiter] = NULL;

                wrapperData->javaArgValueCount = argc - delimiter - 1;
                if (delimiter + 1 < argc) {
                    wrapperData->javaArgValues = &argv[delimiter + 1];
                }
                break;
            }
        }
    }

    /* Store the name of the binary.*/
    wrapperData->argBinary = argv[0];

    wrapperData->baseName = malloc(sizeof(TCHAR) * (_tcslen(argv[0]) + 1));
    if (!wrapperData->baseName) {
        outOfMemory(TEXT("WPA"), 2);
        return FALSE;
    }
    wrapperGetFileBase(argv[0], &wrapperData->baseName);

    wrapperArgCount = delimiter;
    if (wrapperArgCount > 1) {
        if (argv[1][0] == TEXT('-')) {
            /* Syntax 1 or 3 */

            /* A command appears to have been specified. */
            wrapperData->argCommand = &argv[1][1]; /* Strip off the '-' */
            if (wrapperData->argCommand[0] == TEXT('\0')) {
                wrapperUsage(argv[0]);
                return FALSE;
            }

            /* Does the argument have a value? */
            c = _tcschr(wrapperData->argCommand, TEXT('='));
            if (c == NULL) {
                wrapperData->argCommandArg = NULL;
            } else {
                wrapperData->argCommandArg = (TCHAR *)(c + 1);
                c[0] = TEXT('\0');
            }

            /* Check if the command is valid. */
            wrapperData->argCommandValid = isLaunchCommandValid(wrapperData->argCommand);

            if (wrapperArgCount > 2) {
                if (_tcscmp(wrapperData->argCommand, TEXT("-translate")) == 0) {
                    if (wrapperArgCount > 3) {
                        wrapperData->argConfFile = argv[3];
                        wrapperData->argCount = wrapperArgCount - 4;
                        wrapperData->argValues = &argv[4];
                    }
                    return TRUE;
                } else if ((_tcscmp(wrapperData->argCommand, TEXT("-jvm_bits")) == 0) ||
#ifdef WIN32
                           (_tcscmp(wrapperData->argCommand, TEXT("-request_log_file")) == 0) ||
                           (_tcscmp(wrapperData->argCommand, TEXT("-request_default_log_file")) == 0) ||
#endif
                           (_tcscmp(wrapperData->argCommand, TEXT("-request_delta_binary_bits")) == 0)) {
                    wrapperData->argConfFile = argv[2];
                    wrapperData->argCount = wrapperArgCount - 3;
                    wrapperData->argValues = &argv[3];
                    
                    /* If no configuration file is specified, we will go to case 'Syntax 3'. */
                    return TRUE;
                }

                /* Syntax 1 */
                /* A command and conf file were specified. */
                wrapperData->argConfFile = argv[2];
                wrapperData->argCount = wrapperArgCount - 3;
                wrapperData->argValues = &argv[3];
            } else {
                /* Syntax 3 */
                /* Only a command was specified.  Assume a default config file name. */
                wrapperSetDefaultConfFile(defaultConfFile);
                wrapperData->argConfFileDefault = TRUE;
                wrapperData->argCount = wrapperArgCount - 2;
                wrapperData->argValues = &argv[2];
            }
        } else {
            /* Syntax 2 */
            /* A command was not specified, but there may be a config file. */
            wrapperData->argCommand = TEXT("c");
            wrapperData->argCommandArg = NULL;
            wrapperData->argCommandValid = TRUE;
            wrapperData->argConfFile = argv[1];
            wrapperData->argCount = wrapperArgCount - 2;
            wrapperData->argValues = &argv[2];
        }
    } else {
        /* Systax 4 */
        /* A config file was not specified.  Assume a default config file name. */
        wrapperData->argCommand = TEXT("c");
        wrapperData->argCommandArg = NULL;
        wrapperData->argCommandValid = TRUE;
        wrapperSetDefaultConfFile(defaultConfFile);
        wrapperData->argConfFileDefault = TRUE;
        wrapperData->argCount = wrapperArgCount - 1;
            wrapperData->argValues = &argv[1];
    }

    return TRUE;
}

/**
 * Performs the specified action,
 *
 * @param actionList An array of action Ids ending with a value ACTION_LIST_END.
 *                   Negative values are standard actions, positive are user
 *                   custom events.
 * @param triggerMsg The reason the actions are being fired.
 * @param actionSourceCode Tracks where the action originated.
 * @param actionPropertyIndex Index of the property where the action was configured. Ignored if the type of action is not configured with a <n>-component property.
 * @param logForActionNone Flag stating whether or not a message should be logged
 *                         for the NONE action.
 * @param exitCode Error code to use in case the action results in a shutdown.
 */
void wrapperProcessActionList(int *actionList, const TCHAR *triggerMsg, int actionSourceCode, int actionPropertyIndex, int logForActionNone, int exitCode) {
    int i;
    int action;
    TCHAR propertyName[52];

    if (actionList) {
        i = 0;
        while ((action = actionList[i]) != ACTION_LIST_END) {
                switch(action) {
                case ACTION_RESTART:
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("%s  %s"), triggerMsg, wrapperGetRestartProcessMessage());
                    wrapperRestartProcess();
                    if (actionSourceCode == WRAPPER_ACTION_SOURCE_CODE_PING_TIMEOUT) {
                        wrapperKillProcess(TRUE);
                    }
                    break;

                case ACTION_SHUTDOWN:
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("%s  Shutting down."), triggerMsg);
                    wrapperData->shutdownActionTriggered = TRUE;
                    switch (actionSourceCode) {
                    case WRAPPER_ACTION_SOURCE_CODE_FILTER:
                        _sntprintf(propertyName, 52, TEXT("wrapper.filter.action.%d"), actionPropertyIndex);
                        break;
                    case WRAPPER_ACTION_SOURCE_CODE_PING_TIMEOUT:
                        _sntprintf(propertyName, 52, TEXT("wrapper.ping.timeout.action"));
                        break;
                    default:
                        _sntprintf(propertyName, 52, TEXT(""));
                    }
                    updateStringValue(&(wrapperData->shutdownActionPropertyName), propertyName);
                    if (actionSourceCode == WRAPPER_ACTION_SOURCE_CODE_PING_TIMEOUT) {
                        wrapperData->exitCode = exitCode;
                        wrapperKillProcess(TRUE);
                    } else {
                        wrapperStopProcess(exitCode, FALSE);
                    }
                    break;

                case ACTION_DUMP:
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("%s  Requesting thread dump."), triggerMsg);
                    wrapperRequestDumpJVMState();
                    break;

                case ACTION_DEBUG:
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("%s  Debugging."), triggerMsg);
                    break;

                case ACTION_PAUSE:
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("%s  %s"), triggerMsg, wrapperGetPauseProcessMessage());
                    wrapperPauseProcess(actionSourceCode);
                    break;

                case ACTION_RESUME:
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("%s  %s"), triggerMsg, wrapperGetResumeProcessMessage());
                    wrapperResumeProcess(actionSourceCode);
                    break;

#if defined(MACOSX)
                case ACTION_ADVICE_NIL_SERVER:
                    if (wrapperData->isAdviserEnabled) {
                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ADVICE, TEXT(""));
                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ADVICE, TEXT(
                            "--------------------------------------------------------------------"));
                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ADVICE, TEXT(
                            "Advice:"));
                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ADVICE, TEXT(
                            "MACOSX is known to have problems displaying GUIs from processes\nrunning as a daemon launched from launchd.  The above\n\"Returning nil _server\" means that you are encountering this\nproblem.  This usually results in a long timeout which is affecting\nthe performance of your application."));
                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ADVICE, TEXT(
                            "--------------------------------------------------------------------"));
                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ADVICE, TEXT(""));
                    }
                    break;
#endif

                case ACTION_NONE:
                    if (logForActionNone) {
                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("%s"), triggerMsg);
                    }
                    /* Do nothing but masks later filters */
                    break;

                case ACTION_SUCCESS:
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("%s  Application has signaled success, consider this application started successful..."), triggerMsg);
                    wrapperData->failedInvocationCount = 0;
                    break;

                case ACTION_GC:
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("%s  Requesting GC..."), triggerMsg);
                    wrapperRequestJVMGC(actionSourceCode);
                    break;

                default:
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN, TEXT("Unknown action type: %d"), action);
                    break;
                }

            i++;
        }
    }
}

/**
 * Function that will recursively attempt to match two strings where the
 *  pattern can contain '?' or '*' wildcard characters.  This function requires
 *  that the pattern be matched from the beginning of the text.
 *
 * @param text Text to be searched.
 * @param textLen Length of the text.
 * @param pattern Pattern to search for.
 * @param patternLen Length of the pattern.
 * @param minTextLen Minimum number of characters that the text needs to possibly match the pattern.
 *
 * @return TRUE if found, FALSE otherwise.
 *
 * 1)     text=abcdefg  textLen=7  pattern=a*d*efg  patternLen=7  minTextLen=5
 * 1.1)   text=bcdefg   textLen=6  pattern=d*efg    patternLen=5  minTextLen=4
 * 1.2)   text=cdefg    textLen=5  pattern=d*efg    patternLen=5  minTextLen=4
 * 1.3)   text=defg     textLen=4  pattern=d*efg    patternLen=5  minTextLen=4
 * 1.3.1) text=efg      textLen=3  pattern=efg      patternLen=3  minTextLen=3
 */
int wildcardMatchInner(const TCHAR *text, size_t textLen, const TCHAR *pattern, size_t patternLen, size_t minTextLen) {
    size_t textIndex;
    size_t patternIndex;
    TCHAR patternChar;
    size_t textIndex2;
    TCHAR textChar;

    /*log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_INFO, TEXT("  wildcardMatchInner(\"%s\", %d, \"%s\", %d, %d)"), text, textLen, pattern, patternLen, minTextLen);*/

    textIndex = 0;
    patternIndex = 0;

    while ((textIndex < textLen) && (patternIndex < patternLen)) {
        patternChar = pattern[patternIndex];

        if (patternChar == TEXT('*')) {
            /* The pattern '*' can match 0 or more characters.  This requires a bit of recursion to work it out. */
            textIndex2 = textIndex;
            /* Loop over all possible starting locations.  We know how many characters are needed to match (minTextLen - patternIndex) so we can stop there. */
            while (textIndex2 < textLen - (minTextLen - (patternIndex + 1))) {
                if (wildcardMatchInner(&(text[textIndex2]), textLen - textIndex2, &(pattern[patternIndex + 1]), patternLen - (patternIndex + 1), minTextLen - patternIndex)) {
                    /* Got a match in recursion. */
                    /*log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_INFO, TEXT("  wildcardMatchInner(\"%s\", %d, \"%s\", %d, %d) -> HERE1 textIndex=%d, patternIndex=%d, textIndex2=%d TRUE"), text, textLen, pattern, patternLen, minTextLen, textIndex, patternIndex, textIndex2);*/
                    return TRUE;
                } else {
                    /* Failed to match.  Try matching one more character against the '*'. */
                    textIndex2++;
                }
            }
            /* If we get here then all possible starting locations failed. */
            /*log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_INFO, TEXT("  wildcardMatchInner(\"%s\", %d, \"%s\", %d, %d) -> HERE2 textIndex=%d, patternIndex=%d, textIndex2=%d FALSE"), text, textLen, pattern, patternLen, minTextLen, textIndex, patternIndex, textIndex2);*/
            return FALSE;
        } else if (patternChar == TEXT('?')) {
            /* Match any character. */
            patternIndex++;
            textIndex++;
        } else {
            textChar = text[textIndex];
            if (patternChar == textChar) {
                /* Characters match. */
                patternIndex++;
                textIndex++;
            } else {
                /* Characters do not match.  We are done. */
                /*log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_INFO, TEXT("  wildcardMatchInner(\"%s\", %d, \"%s\", %d, %d) -> HERE3 textIndex=%d, patternIndex=%d FALSE"), text, textLen, pattern, patternLen, minTextLen, textIndex, patternIndex);*/
                return FALSE;
            }
        }
    }

    /* It is ok if there are text characters left over as we only need to match a substring, not the whole string. */

    /* If there are any pattern chars left.  Make sure that they are all wildcards. */
    while (patternIndex < patternLen) {
        if (pattern[patternIndex] != TEXT('*')) {
            /*log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_INFO, TEXT("  wildcardMatchInner(\"%s\", %d, \"%s\", %d, %d) -> HERE4 pattern[%d]=%c FALSE"), text, textLen, pattern, patternLen, minTextLen, patternIndex, pattern[patternIndex]);*/
            return FALSE;
        }
        patternIndex++;
    }

    /*log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_INFO, TEXT("  wildcardMatchInner(\"%s\", %d, \"%s\", %d, %d) -> HERE5 textIndex=%d, patternIndex=%d TRUE"), text, textLen, pattern, patternLen, minTextLen, textIndex, patternIndex);*/
    return TRUE;
}
    
/**
 * Does any necessary post processing on the command string.
 *  This function assumes that command has been malloced.  It will either return
 *  the string as is, or return a modified string.  When a modified string is
 *  returned the orignal command buffer will always be freed.
 *
 * 1) Replace the first instance of the %WRAPPER_COMMAND_FILLER_N% environment
 *    variable so that the total command length will be equal to or greater than
 *    the length specified by N.  The padding will be the length plus a series of
 *    Xs terminated by a single Y.  This is mainly for testing.
 *
 * @param command The original command.
 *
 * @return The modifed command.
 */
TCHAR *wrapperPostProcessCommandElement(TCHAR *command) {
    TCHAR *pos1;
    TCHAR *pos2;
    size_t commandLen;
    size_t commandLen2;
    size_t commandLenLen;
    TCHAR commandLenBuffer[8];
    size_t fillerLen;
    TCHAR *tempCommand;
    size_t index;
    
    /* If the special WRAPPER_COMMAND_FILLER_N environment variable is being used then expand it.
     *  This is mainly used for testing. */
    pos1 = _tcsstr(command, TEXT("%WRAPPER_COMMAND_FILLER_"));
    if (pos1 == NULL) {
        return command;
    }
    
    pos2 = _tcsstr(pos1 + 1, TEXT("%"));
    if (pos2 == NULL) {
        return command;
    }
    
    commandLen = _tcslen(command);
    commandLenLen = pos2 - pos1 - 24;
    if (commandLenLen >= 8) {
        /* Too long. invalid. */
        return command;
    }

    memcpy(commandLenBuffer, pos1 + 24, sizeof(TCHAR) * commandLenLen);
    commandLenBuffer[commandLenLen] = TEXT('\0');
    commandLen2 = __max((int)(commandLen - commandLenLen) - 25, __min(_ttoi(commandLenBuffer), 9999999));
    
    fillerLen = commandLen2 - commandLen + commandLenLen + 25;
    
    tempCommand = malloc(sizeof(TCHAR) * (commandLen - commandLenLen - 25 + fillerLen + 1));
    if (!tempCommand) {
        outOfMemory(TEXT("WBJC"), 3);
        return command;
    }
    
    memcpy(tempCommand, command, (pos1 - command) * sizeof(TCHAR));
    index = pos1 - command;
    if (fillerLen > 11) {
        _sntprintf(&(tempCommand[index]), commandLen2 + 1 - index, TEXT("FILL-%d-"), fillerLen);
        fillerLen -= _tcslen(&tempCommand[index]);
        index += _tcslen(&tempCommand[index]);
    }
    while (fillerLen > 1) {
        tempCommand[index] = TEXT('X');
        index++;
        fillerLen--;
    }
    if (fillerLen > 0) {
        tempCommand[index] = TEXT('Y');
        index++;
        fillerLen--;
    }
    memcpy(&(tempCommand[index]), pos2 + 1, sizeof(TCHAR) * _tcslen(pos2 + 1));
    tempCommand[commandLen2] = TEXT('\0');
    
    free(command);
    
    return tempCommand;
}

/**
 * Test function to pause the current thread for the specified amount of time.
 *  This is used to test how the rest of the Wrapper behaves when a particular
 *  thread blocks for any reason.
 *
 * @param pauseTime Number of seconds to pause for.  -1 will pause indefinitely.
 * @param threadName Name of the thread that will be logged prior to pausing.
 */
void wrapperPauseThread(int pauseTime, const TCHAR *threadName) {
    int i;
    
    if (pauseTime > 0) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN, TEXT("Pausing the \"%s\" thread for %d seconds..."), threadName, pauseTime);
        for (i = 0; i < pauseTime; i++) {
            wrapperSleep(1000);
        }
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN, TEXT("Resuming the \"%s\" thread..."), threadName);
    } else if (pauseTime < 0) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN, TEXT("Pausing the \"%s\" thread indefinitely."), threadName);
        while(TRUE) {
            wrapperSleep(1000);
        }
    }
}

/**
 * Function that will recursively attempt to match two strings where the
 *  pattern can contain '?' or '*' wildcard characters.
 *
 * @param text Text to be searched.
 * @param pattern Pattern to search for.
 * @param patternLen Length of the pattern.
 * @param minTextLen Minimum number of characters that the text needs to possibly match the pattern.
 *
 * @return TRUE if found, FALSE otherwise.
 */
int wrapperWildcardMatch(const TCHAR *text, const TCHAR *pattern, size_t minTextLen) {
    size_t textLen;
    size_t patternLen;
    size_t textIndex;

    /*log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_INFO, TEXT("wrapperWildcardMatch(\"%s\", \"%s\", %d)"), text, pattern, minTextLen);*/

    textLen = _tcslen(text);
    if (textLen < minTextLen) {
        return FALSE;
    }

    patternLen = _tcslen(pattern);
    /*log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_INFO, TEXT("  textLen=%d, patternLen=%d"), textLen, patternLen);*/

    textIndex = 0;
    while (textIndex <= textLen - minTextLen) {
        if (wildcardMatchInner(&(text[textIndex]), textLen - textIndex, pattern, patternLen, minTextLen)) {
            return TRUE;
        }
        textIndex++;
    }

    return FALSE;
}

/**
 * Calculates the minimum text length which could be matched by the specified pattern.
 *  Patterns can contain '*' or '?' wildcards.
 *  '*' matches 0 or more characters.
 *  '?' matches exactly one character.
 *
 * @param pattern Pattern to calculate.
 *
 * @return The minimum text length of the pattern.
 */
size_t wrapperGetMinimumTextLengthForPattern(const TCHAR *pattern) {
    size_t patternLen;
    size_t patternIndex;
    size_t minLen;

    /*log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_INFO, TEXT("wrapperGetMinimumTextLengthForPattern(%s)"), pattern);*/

    patternLen = _tcslen(pattern);
    minLen = 0;
    for (patternIndex = 0; patternIndex < patternLen; patternIndex++) {
        if (pattern[patternIndex] == TEXT('*')) {
            /* Matches 0 or more characters, so don't increment the minLen */
        } else {
            minLen++;
        }
    }

    /*log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_INFO, TEXT("wrapperGetMinimumTextLengthForPattern(%s) -> %d"), pattern, minLen);*/

    return minLen;
}

/**
 * Trims any whitespace from the beginning and end of the in string
 *  and places the results in the out buffer.  Assumes that the out
 *  buffer is at least as large as the in buffer. */
void trim(const TCHAR *in, TCHAR *out) {
    size_t len;
    size_t first;
    size_t last;

    len = _tcslen(in);
    if (len > 0) {
        first = 0;
        last = len - 1;

        /* Left Trim */
        while (((in[first] == ' ') || (in[first] == '\t')) && (first < last)) {
            first++;
        }
        /* Right Trim */
        while ((last > first) && ((in[last] == ' ') || (in[last] == '\t'))) {
            last--;
        }

        /* Copy over what is left. */
        len = last - first + 1;
        if (len > 0) {
            _tcsncpy(out, in + first, len);
        }
    }
    out[len] = TEXT('\0');
}

/**
 * Comfirm that the Java version is in the range in which the Wrapper is allowed to run.
 *
 * @return TRUE if the Java version is ok, FALSE otherwise.
 */
int wrapperConfirmJavaVersion() {
    int result = TRUE;
    JavaVersion *minVersion1;
    JavaVersion *minVersion2;
    JavaVersion *maxVersion;
    const TCHAR *minVersionName = NULL;
    
    /* wrapper.java.version.min & wrapper.java.version.min can't be less than the minimum version of Java supported by the Wrapper. */
    minVersion1 = getMinRequiredJavaVersion();
    minVersion2 = minVersion1;
    
    if (wrapperData->javaVersionMin) {
        disposeJavaVersion(wrapperData->javaVersionMin);
    }
    wrapperData->javaVersionMin = getJavaVersionProperty(TEXT("wrapper.java.version.min"), minVersion1->displayName, minVersion1, NULL, 0);
    if (!wrapperData->javaVersionMin) {
        /* Invalid configuration. A FATAL error has been logged. */
        result = FALSE;
    } else if (compareJavaVersion(wrapperData->javaVersionMin, minVersion2) > 0) {
        /* wrapper.java.version.max can't be less than wrapper.java.version.min. */
        minVersion2 = wrapperData->javaVersionMin;
        minVersionName = TEXT("wrapper.java.version.min");
    }
    
    if (wrapperData->javaVersionMax) {
        disposeJavaVersion(wrapperData->javaVersionMax);
    }
    wrapperData->javaVersionMax = getJavaVersionProperty(TEXT("wrapper.java.version.max"), TEXT("UNLIMITED"), minVersion2, minVersionName, UINT_MAX);
    if (!wrapperData->javaVersionMax) {
        /* Invalid configuration. A FATAL error has been logged. */
        result = FALSE;
    }
    
    if (result) {
        if (!wrapperData->javaVersion) {
            /* This should never happen. */
            result = FALSE;
        } else if (wrapperData->javaVersion->isUnknown) {
            maxVersion = getMaxRequiredJavaVersion();
            if ((compareJavaVersion(wrapperData->javaVersionMin, minVersion1) != 0) || (compareJavaVersion(wrapperData->javaVersionMax, maxVersion) != 0)) {
                /* We previously failed to parse the Java version currently used, so if wrapper.java.version.min or wrapper.java.version.max
                 *  are not set to their defaults, we should stop. Otherwise continue to not risk blocking the Wrapper for certain JVMs. */
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, 
                    TEXT("Cannot confirm the version of Java. Usage of %s\n  and %s will prevent the Wrapper from continuing."),
                    TEXT("wrapper.java.version.min"), TEXT("wrapper.java.version.max"));
                result = FALSE;
            }
            disposeJavaVersion(maxVersion);
        } else {
            /* If the configuration is correct, confirm that the version Java is between the min and the max. */
            if (compareJavaVersion(wrapperData->javaVersion, wrapperData->javaVersionMin) < 0) {
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("The version of Java specified by wrapper.java.command (%s)\n is lower than the minimum required (%s)."),
                    wrapperData->javaVersion->displayName, wrapperData->javaVersionMin->displayName);
                result = FALSE;
            } else if (compareJavaVersion(wrapperData->javaVersion, wrapperData->javaVersionMax) > 0) {
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("The version of Java specified by wrapper.java.command (%s)\n is greater than the maximum allowed (%s)."),
                    wrapperData->javaVersion->displayName, wrapperData->javaVersionMax->displayName);
                result = FALSE;
            }
        }
    }
    disposeJavaVersion(minVersion1);
    return result;
}

/**
 * Set the Java version that the Wrapper will use.
 *
 * @param javaVersion The Java version to set or NULL if a default value should be set.
 */
void wrapperSetJavaVersion(JavaVersion* javaVersion) {
    JavaVersion *fallbackVersion;
    JavaVersion *minRequiredVersion;

    if (wrapperData->javaVersion) {
        disposeJavaVersion(wrapperData->javaVersion);
        wrapperData->javaVersion = NULL;
    }
    if (javaVersion) {
        wrapperData->javaVersion = javaVersion;
    } else {
        minRequiredVersion = getMinRequiredJavaVersion();
        fallbackVersion = getJavaVersionProperty(TEXT("wrapper.java.version.fallback"), NULL, minRequiredVersion, NULL, 0);
        if (fallbackVersion) {
            wrapperData->javaVersion = fallbackVersion;
            disposeJavaVersion(minRequiredVersion);
            log_printf_queue(TRUE, WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR,
                TEXT("Failed to parse the version of Java. Resolving to the value of wrapper.java.version.fallback (%s)."),
                fallbackVersion->displayName);
        } else {
            /* We are in a void function. We will handle the error in the main eventloop if wrapperData->javaVersion is NULL. */
            if (getNotEmptyStringProperty(properties, TEXT("wrapper.java.version.fallback"), NULL)) {
                /* 'wrapper.java.version.fallback' was specified in the configuration but
                 *  getJavaVersionProperty() returned NULL. That means we failed to parse the value. */
                wrapperData->javaVersion = NULL;
                disposeJavaVersion(minRequiredVersion);
            } else {
                /* Resolve to the minimum required version. NULL is unlikely to happen but
                 *  would be handled in the main eventloop. */
                wrapperData->javaVersion = minRequiredVersion;
                if (wrapperData->javaVersion) {
                    log_printf_queue(TRUE, WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR,
                        TEXT("Failed to parse the version of Java. Resolving to the lowest supported version (%s)."),
                        wrapperData->javaVersion->displayName);
                    wrapperData->javaVersion->isUnknown = TRUE;
                }
            }
        }
        if (!wrapperData->printJVMVersion && (getLogfileLevelInt() > wrapperData->jvmDefaultLogLevel)) {
            /* Note: In theory we should also check that the logfile loglevel is INFO or less, but then we would need a different advice for each case... */
            log_printf_queue(TRUE, WRAPPER_SOURCE_WRAPPER, LEVEL_ADVICE,
                TEXT("  Please set wrapper.java.version.output to TRUE, relaunch the Wrapper to print out the Java version output, and send the log file to support@tanukisoftware.com."));
        }
    }
    if (wrapperData->javaVersion) {
        log_printf_queue(TRUE, WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Java version: %d.%d.%d"),
            wrapperData->javaVersion->major, wrapperData->javaVersion->minor, wrapperData->javaVersion->revision);
    }
}

static int javaVersionCurrentParseLine;
static int javaVersionParseLine;    /* The line at which the version of Java can be found in the 'java -version' output. */
static int javaVendorParseLine;     /* The line at which the JVM maker can be found in the 'java -version' output. */
static int javaBitsParseLine;       /* The line at which the JVM bits can be found in the 'java -version' output. */

void initJavaVersionParser() {
    javaVersionCurrentParseLine = 0;
    
    /* Reset the default lines at which the version, vendor and bits are supposed to appear. */
    javaVersionParseLine = 1;
    javaVendorParseLine = 3;
    javaBitsParseLine = 3;
}

int javaVersionFound() {
    return (javaVersionCurrentParseLine >= javaVersionParseLine);
}

void logParseJavaVersionOutput(TCHAR* log) {
    JavaVersion *javaVersion;
    
    javaVersionCurrentParseLine++;
    
    /* Queue the messages to avoid logging it in the middle of the JVM output. */
    if (javaVersionCurrentParseLine == javaVersionParseLine) {
        
        if (!_tcsstr(log, TEXT("version \""))) {
            /* This is not the line containing the version. This can happen when a system message is inserted before the Java output. */
            javaVersionParseLine++;
            javaVendorParseLine++;
            javaBitsParseLine++;
            return;
        } else {
            /* Parse the Java version. */
            javaVersion = parseOutputJavaVersion(log);
            wrapperSetJavaVersion(javaVersion);
        }
    }
    if (javaVersionCurrentParseLine == javaVendorParseLine) {
        /* Parse the JVM maker (JVM implementation). */
        wrapperData->jvmVendor = parseOutputJvmVendor(log);
        log_printf_queue(TRUE, WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Java vendor: %s"), getJvmVendorName(wrapperData->jvmVendor));
    }
    if (javaVersionCurrentParseLine == javaBitsParseLine) {
        /* Parse the JVM bits. */
        wrapperData->jvmBits = parseOutputJvmBits(log, wrapperData->javaVersion);
        log_printf_queue(TRUE, WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Java bits: %s"), getJvmBitsName(wrapperData->jvmBits));
    }
    if (javaVersionCurrentParseLine >= __max(__max(javaVersionParseLine, javaVendorParseLine), javaBitsParseLine)) {
        wrapperData->jvmQueryEvaluated = TRUE;
    }
}

void initJavaBootstrapParser() {
    wrapperData->jvmBootstrapFailed = FALSE;
    wrapperData->jvmBootstrapVersionOk = FALSE;
    wrapperData->jvmBootstrapVersionMismatch = FALSE;
}

void logParseJavaBootstrapOutput(TCHAR* log) {
    TCHAR* pKey = NULL;
    TCHAR* pVal = NULL;
    int parsed = FALSE;
    int skip = FALSE;

    if (_tcsstr(log, TEXT("WrapperBootstrap: ")) == log) {
        pKey = log + 18;
    } else {
        /* skip parsing this output */
        /*  Note: The "WrapperBootstrap: " prefix was added in 3.5.60. Still attempt to check the version in case this is an older jar file. */
        pKey = log;
        skip = TRUE;
    }

    if (!wrapperData->jvmBootstrapVersionOk) {
        /* 'wrapper_version: ...' should be the first line of output, unless:
         *   - the JVM failed to launch (messages from the JVM will be printed before the output - can be confirmed with a '--dry-run' when running java 9+)
         *   - some debug output is printed (due to options set with 'BOOTSTRAP_DRYRUN_APP')
         *   - wrapper.jarfile has been set to an unofficial jar file which is using the main class with the '...WrapperBootstrap' name.
         *   - in some rare cases (for example, ulimit warnings), the operating system may add a warning header whenever a child process is launched. */
        if (_tcsstr(pKey, TEXT("wrapper_version: ")) == pKey) {
            pVal = pKey + 17;
            if (_tcscmp(pVal, wrapperVersionRoot) == 0) {
                wrapperData->jvmBootstrapVersionOk = TRUE;
            } else {
                log_printf_queue(TRUE, WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("The version of the Wrapper jar file (%s) doesn't match the version of this Wrapper (%s)."), pVal, wrapperVersionRoot);
                wrapperData->jvmBootstrapFailed = TRUE;
                wrapperData->jvmBootstrapVersionMismatch = TRUE;
            }
        }
    } else if (!skip) {
        switch (wrapperData->jvmBootstrapMode) {
        case BOOTSTRAP_ENTRYPOINT_MAINCLASS:
            if (_tcsstr(pKey, TEXT("mainclass: ")) == pKey) {
                pVal = pKey + 11;
                if (_tcsstr(pVal, TEXT("not found")) == pVal) {
                    log_printf_queue(TRUE, WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Main class '%s' not found.  Please check that it is correctly spelled and that it can be found in the classpath or module path."), wrapperData->mainUsrClass);
                    wrapperData->jvmBootstrapFailed = TRUE;
                    parsed = TRUE;
                } else if (_tcsstr(pVal, TEXT("load failed")) == pVal) {
                    log_printf_queue(TRUE, WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Main class '%s' could not be loaded."), wrapperData->mainUsrClass);
                    wrapperData->jvmBootstrapFailed = TRUE;
                    /* The call stack will be printed. */
                    wrapperData->jvmDefaultLogLevel = LEVEL_ERROR;
                    parsed = TRUE;
                }
            }
            break;

     /* case BOOTSTRAP_ENTRYPOINT_MODULE:
            if (_tcsstr(pKey, TEXT("mainmodule: ")) == pKey) {
                pVal = pKey + 12;
                if (_tcsstr(pVal, TEXT("not found")) == pVal) {
                    log_printf_queue(TRUE, WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Main module '%s' not found.  Please check that the wrapper.java.module_path.<n> and wrapper.java.module.<n> properties are correctly set."), wrapperData->mainModule);
                    wrapperData->jvmBootstrapFailed = TRUE;
                } else if (_tcsstr(pVal, TEXT("no main class")) == pVal) {
                    log_printf_queue(TRUE, WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("No main class for module '%s'."), wrapperData->mainModule);
                    wrapperData->jvmBootstrapFailed = TRUE;
                }
                parsed = TRUE;
            } else if (_tcsstr(pKey, TEXT("mainclass: ")) == pKey) {
                pVal = pKey + 11;
                if (_tcsstr(pVal, TEXT("no main class")) == pVal) {
                    log_printf_queue(TRUE, WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Main class '%s' of module '%s' doesn't exist or is not accessible.  Please check that it is exists in the modulepath."), wrapperData->mainUsrClass, wrapperData->mainModule);
                    wrapperData->jvmBootstrapFailed = TRUE;
                } else {
                    updateStringValue(&wrapperData->mainUsrClass, pVal);
                }
                parsed = TRUE;
            }
            break; */

        case BOOTSTRAP_ENTRYPOINT_JAR:
            if (_tcsstr(pKey, TEXT("mainjar: ")) == pKey) {
                pVal = pKey + 9;
                if (_tcsstr(pVal, TEXT("not found")) == pVal) {
                    log_printf_queue(TRUE, WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Unable to locate the jar file '%s'.  Please check that the %s property is correctly set."), wrapperData->mainJar, TEXT("wrapper.app.parameter.1"));
                    wrapperData->jvmBootstrapFailed = TRUE;
                } else if (_tcsstr(pVal, TEXT("can't open")) == pVal) {
                    log_printf_queue(TRUE, WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Unable to open the jar file '%s'.  Please check that it has appropriate file permissions."), wrapperData->mainJar);
                    wrapperData->jvmBootstrapFailed = TRUE;
                } else if (_tcsstr(pVal, TEXT("access denied")) == pVal) {
                    log_printf_queue(TRUE, WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Unable to open the jar file '%s' due to access denied by the SecurityManager."), wrapperData->mainJar);
                    wrapperData->jvmBootstrapFailed = TRUE;
                } else if (_tcsstr(pVal, TEXT("no manifest")) == pVal) {
                    log_printf_queue(TRUE, WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Unable to access the manifest file of %s."), wrapperData->mainJar);
                    wrapperData->jvmBootstrapFailed = TRUE;
                } else if ((_tcsstr(pVal, TEXT("no mainclass")) == pVal) && !wrapperData->mainUsrClass) {
                    log_printf_queue(TRUE, WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("The %s was not specified correctly in the manifest file of %s.  Please make sure all required meta information is being set."), TEXT("Main-Class"), wrapperData->mainJar);
                    wrapperData->jvmBootstrapFailed = TRUE;
/*              } else if ((_tcsstr(pVal, TEXT("no classpath")) == pVal) && !wrapperData->mainUsrClass) {
                    log_printf_queue(TRUE, WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("The %s was not specified correctly in the manifest file of %s.  Please make sure all required meta information is being set."), TEXT("Class-Path"), wrapperData->mainJar);
                    wrapperData->jvmBootstrapFailed = TRUE; */
                }
                parsed = TRUE;
            } else if (_tcsstr(pKey, TEXT("mainclass: ")) == pKey) {
                pVal = pKey + 11;
                if (_tcsstr(pVal, TEXT("not found - ")) == pVal) {
                    pVal += 12;
                    log_printf_queue(TRUE, WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Unable to locate the main class '%s' of %s."), pVal, wrapperData->mainJar);
                    wrapperData->jvmBootstrapFailed = TRUE;
                } else {
                    updateStringValue(&wrapperData->mainUsrClass, pVal);
                }
                parsed = TRUE;
/*          } else if ((_tcsstr(pKey, TEXT("classpath: ")) == pKey)) {
                pVal = pKey + 11;
                updateStringValue(&wrapperData->jarClassPath, pVal); */
                parsed = TRUE;
            }
            break;
        }

        if (!parsed && wrapperData->jvmAddOpens) {
            /* Note: Normally WrapperBootstrap should only output 'mainmodule: ' or 'package: ' if the main class was found,
             *       but just to be safe, print '<not_found>' if wrapperData->mainUsrClass is null. */
            if (_tcsstr(pKey, TEXT("mainmodule: ")) == pKey) {
                pVal = pKey + 12;
                if (_tcsstr(pVal, TEXT("not found")) == pVal) {
                    log_printf_queue(TRUE, WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Module of main class '%s' not found."),
                                                                                wrapperData->mainUsrClass ? wrapperData->mainUsrClass : TEXT("<not_found>"));
                    wrapperData->jvmBootstrapFailed = TRUE;
                } else if (_tcsstr(pVal, TEXT("unnamed")) == pVal) {
                    wrapperData->jvmAddOpens = FALSE;
                } else {
                    updateStringValue(&wrapperData->mainModule, pVal);
                    if (!wrapperData->mainModule) {
                        wrapperData->jvmBootstrapFailed = TRUE;
                    }
                }
                parsed = TRUE;
            } else if (_tcsstr(pKey, TEXT("package: ")) == pKey) {
                /* Note: For the package, an alternative to parsing the bootstrap output would be to use all characters before the last '.' in the main class name.
                 *       This should work even for a nested static class. After compilation, its bytecode class name (which is the form to use in the command line
                 *       and therefore in the configuration file) would have any period replaced by '$'. This guarantees that only package names can contain periods. */
                pVal = pKey + 9;
                if (_tcsstr(pVal, TEXT("not found")) == pVal) {
                    log_printf_queue(TRUE, WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Failed to retrieve package for class '%s'."),
                                                                                wrapperData->mainUsrClass ? wrapperData->mainUsrClass : TEXT("<not_found>"));
                    wrapperData->jvmBootstrapFailed = TRUE;
                } else {
                    updateStringValue(&wrapperData->mainUsrPkg, pVal);
                    if (!wrapperData->mainUsrPkg) {
                        wrapperData->jvmBootstrapFailed = TRUE;
                    }
                }
                parsed = TRUE;
            }
        }

        if (!parsed) {
            if ((_tcsstr(pKey, TEXT("invalid_argument")) == pKey) || (_tcsstr(pKey, TEXT("wrong_argument_number")) == pKey)) {
                log_printf_queue(TRUE, WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Incorrect call to WrapperBootstrap class."));
                wrapperData->jvmBootstrapFailed = TRUE;
                /* Let WrapperBootstrap print its error. */
                wrapperData->jvmDefaultLogLevel = LEVEL_ERROR;
            }
        }

        if (!parsed) {
            if (_tcscmp(pKey, TEXT("--")) == 0) {
                /* End token, which means all the information is collected. */
                wrapperData->jvmQueryEvaluated = TRUE;
            }
        }
    }
}

void logApplyFilters(const TCHAR *log) {
    int i;
    const TCHAR *filter;
    const TCHAR *filterMessage;
    int matched;

    /* Look for output filters in the output.  Only match the first. */
    for (i = 0; i < wrapperData->outputFilterCount; i++) {
        if (_tcslen(wrapperData->outputFilters[i]) > 0) {
            /* The filter is defined. */
            matched = FALSE;
            filter = wrapperData->outputFilters[i];

            if (wrapperData->outputFilterAllowWildFlags[i]) {
                if (wrapperWildcardMatch(log, filter, wrapperData->outputFilterMinLens[i])) {
                    matched = TRUE;
                }
            } else {
                /* Do a simple check to see if the pattern is found exactly as is. */
                if (_tcsstr(log, filter)) {
                    /* Found an exact match for the pattern. */
                    /* Any wildcards in the pattern can be matched exactly if they exist in the output.  This is by design. */
                    matched = TRUE;
                }
            }

            if (matched) {
                filterMessage = wrapperData->outputFilterMessages[i];
                if ((!filterMessage) || (_tcslen(filterMessage) <= 0)) {
                    filterMessage = TEXT("Filter trigger matched.");
                }
                wrapperProcessActionList(wrapperData->outputFilterActionLists[i], filterMessage, WRAPPER_ACTION_SOURCE_CODE_FILTER, i, FALSE, wrapperData->errorExitCode);

                /* break out of the loop */
                break;
            }
        }
    }
}

#ifdef _DEBUG
void printBytes(const char * s) {
    TCHAR buffer[MAX_LOG_SIZE];
    TCHAR *pBuffer;
    size_t len = __min(strlen(s), MAX_LOG_SIZE/3);
    size_t i;
    
    buffer[0] = 0;
    pBuffer = buffer;
    for (i = 0; i < len; i++) {
        _sntprintf(pBuffer, 4, TEXT("%02x "), s[i] & 0xff);
        pBuffer +=3;
    }
    buffer[MAX_LOG_SIZE-1] = 0; 
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_INFO, buffer);
}
#endif

/**
 * Logs a single line of child output allowing any filtering
 *  to be done in a common location.
 */
void logChildOutput(const char* log) {
    TCHAR* tlog = NULL;
#ifdef UNICODE
 #ifdef WIN32
    int size;
    UINT cp;
 #endif
#endif

#ifdef _DEBUG
    printBytes(log);
#endif

#ifdef UNICODE
 #ifdef WIN32
    cp = getJvmOutputCodePage();
    size = MultiByteToWideChar(cp, 0, log, -1 , NULL, 0);
    if (size <= 0) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN,
                    TEXT("Invalid multibyte sequence in %s: %s"), TEXT("JVM console output"), getLastErrorText());
        return;
    }

    tlog = (TCHAR*)malloc((size + 1) * sizeof(TCHAR));
    if (!tlog) {
        outOfMemory(TEXT("WLCO"), 1);
        return;
    }
    MultiByteToWideChar(cp, 0, log, -1, tlog, size + 1);
 #else
    if (converterMBToWide(log, getJvmOutputEncodingMB(), &tlog, TRUE)) {
        if (tlog) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN, TEXT("%s"), tlog);
            free(tlog);
        } else {
            outOfMemory(TEXT("WLCO"), 1);
        }
        return;
    }
 #endif
#else
    tlog = (TCHAR*)log;
#endif
    
    /* NOTE: Don't use 'TEXT("%s")' here! log_printf operates in a different (& faster) mode when the source is WRAPPER_SOURCE_JVM_QRY
     *       or a positive integer (jvm restart count). It prints the output as a direct message without processing format specifiers. */
    switch (wrapperData->jvmCallType) {
    case WRAPPER_JVM_VERSION:
        /* tlog will be modified by this call. Make sure it will not be used after that. */
        log_printf(WRAPPER_SOURCE_JVM_QRY, wrapperData->jvmDefaultLogLevel, tlog);
        if (!wrapperData->jvmQuerySkipParse) {
            logParseJavaVersionOutput(tlog);
        }
        break;

    case WRAPPER_JVM_BOOTSTRAP:
        /* It is important to print this line before parsing. Certain tokens are followed by Java errors or call stacks that 
         *  we want to print with the ERROR log level. The parsing of the token will raise the log level, but the token itself
         *  should be logged with the default log level. */
        log_printf(WRAPPER_SOURCE_JVM_QRY, wrapperData->jvmDefaultLogLevel, tlog);
        if (!wrapperData->jvmQuerySkipParse && !wrapperData->jvmBootstrapFailed) {
            logParseJavaBootstrapOutput(tlog);
        }
        break;

    case WRAPPER_JVM_DRY:
        if (!wrapperData->jvmQueryCompleted) {
            log_printf(WRAPPER_SOURCE_JVM_QRY, LEVEL_INFO, tlog);
        } else if (wrapperData->jvmQueryExitCode > 0) {
            /* The errors printed by the dry-run instance are the same as those that would be displayed if the application were launched,
             *  but the Wrapper will stop before launching the application. When the --dry-run command line is not printed, the output
             *  and the message that follows should be clearer if the source is "jvm n" as if it were the real JVM. */
            if ((getLowLogLevel() <= wrapperData->javaQueryLogLevel) && (wrapperData->javaQueryLogLevel != LEVEL_NONE)) {
                log_printf(WRAPPER_SOURCE_JVM_QRY, LEVEL_FATAL, tlog);
            } else {
                log_printf(wrapperData->jvmRestarts + 1, LEVEL_FATAL, tlog);
            }
        } else {
            /* The Wrapper will continue, so print with WRAPPER_SOURCE_JVM_QRY otherwise it would look like the jvm has duplicate output. */
            log_printf(WRAPPER_SOURCE_JVM_QRY, wrapperData->jvmDefaultLogLevel, tlog);
        }
        break;

    case WRAPPER_JVM_APP:
        /* Normal JVM output. */
        log_printf(wrapperData->jvmRestarts, wrapperData->jvmDefaultLogLevel, tlog);

        /* Look for output filters in the output.  Only match the first. */
        logApplyFilters(tlog);
        break;

    default:
        /* Unknown source! -> ignore */
        break;
    }

#ifdef UNICODE
    free(tlog);
#endif
}

/**
 * This function is for moving a buffer inside itself.
 *
 * This implementation exists because the standard memcpy is not reliable on
 *  some platforms when the source and target buffer are the same.  Most likely the
 *  problem was caused by internal optimizations, but it was leading to crashes.
 */
void safeMemCpy(char *buffer, size_t target, size_t src, size_t nbyte) {
    size_t i;
    for (i = 0; i < nbyte; i++) {
        buffer[target + i] = buffer[src + i];
    }
}

#define CHAR_LF 0x0a

/**
 * Read and process any output from the child JVM Process.
 *
 * When maxTimeMS is non-zero this function will only be allowed to run for that maximum
 *  amount of time.  This is done to make sure the calling function is allowed CPU for
 *  other activities.   When timing out for this reason when there is more data in the
 *  pipe, this function will return TRUE to let the calling code know that it should
 *  not do any unnecessary sleeps.  Otherwise FALSE will be returned.
 *
 * @param maxTimeMS The maximum number of milliseconds that this function will be allowed
 *                  to run without returning.  In reality no new reads will happen after
 *                  this time, but actual processing may take longer.
 *
 * @return TRUE if the calling code should call this function again as soon as possible.
 */
int wrapperReadChildOutput(int maxTimeMS) {
    struct timeb timeBuffer;
    time_t startTime;
    int startTimeMillis;
    time_t now;
    int nowMillis;
    time_t durr;
    char *tempBuffer;
    char *cLF;
    int currentBlockRead;
    size_t loggedOffset;
    int defer = FALSE;
    int readThisPass = FALSE;
    size_t i;

    if (!wrapperChildWorkBuffer) {
        /* Initialize the wrapperChildWorkBuffer.  Set its initial size to the block size + 1.
         *  This is so that we can always add a \0 to the end of it. */
        wrapperChildWorkBuffer = malloc(sizeof(char) * ((READ_BUFFER_BLOCK_SIZE * 2) + 1));
        if (!wrapperChildWorkBuffer) {
            outOfMemory(TEXT("WRCO"), 1);
            return FALSE;
        }
        wrapperChildWorkBufferSize = READ_BUFFER_BLOCK_SIZE * 2;
        wrapperChildWorkBufferLen = 0;
    }

    wrapperGetCurrentTime(&timeBuffer);
    startTime = now = timeBuffer.time;
    startTimeMillis = nowMillis = timeBuffer.millitm;

#ifdef DEBUG_CHILD_OUTPUT
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_INFO, TEXT("wrapperReadChildOutput() BEGIN"));
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_INFO, TEXT("now=%ld, nowMillis=%d"), now, nowMillis);
#endif

    /* Loop and read in CHILD_BLOCK_SIZE characters at a time.
     *
     * To keep a JVM outputting lots of content from freezing the Wrapper, we force a return every 250ms. */
    while ((maxTimeMS <= 0) || ((durr = (now - startTime) * 1000 + (nowMillis - startTimeMillis)) < maxTimeMS)) {
#ifdef DEBUG_CHILD_OUTPUT
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_INFO, TEXT("durr=%ld"), durr);
#endif

        /* If there is not enough space in the work buffer to read in a full block then it needs to be extended. */
        if (wrapperChildWorkBufferLen + READ_BUFFER_BLOCK_SIZE > wrapperChildWorkBufferSize) {
#ifdef DEBUG_CHILD_OUTPUT
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_INFO, TEXT("Expand buffer."));
#endif
            /* Increase the buffer quickly, but try not to get too big.  Increase to a size that is the
             *  greater of size + 1024 or size * 1.1.
             * Also make sure the new buffer is larger than the buffer len.  This should not be necessary
             *  but is safer. */
            wrapperChildWorkBufferSize = __max(wrapperChildWorkBufferLen + 1, __max(wrapperChildWorkBufferSize + READ_BUFFER_BLOCK_SIZE, wrapperChildWorkBufferSize + wrapperChildWorkBufferSize / 10));
            
            tempBuffer = malloc(wrapperChildWorkBufferSize + 1);
            if (!tempBuffer) {
                outOfMemory(TEXT("WRCO"), 2);
                return FALSE;
            }
            memcpy(tempBuffer, wrapperChildWorkBuffer, wrapperChildWorkBufferLen);
            tempBuffer[wrapperChildWorkBufferLen] = '\0';
            free(wrapperChildWorkBuffer);
            wrapperChildWorkBuffer = tempBuffer;
#ifdef DEBUG_CHILD_OUTPUT
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_INFO, TEXT("buffer now %d bytes"), wrapperChildWorkBufferSize);
#endif
        }

#ifdef DEBUG_CHILD_OUTPUT
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_INFO, TEXT("Try reading from pipe.  totalBuffLen=%d, buffSize=%d"), wrapperChildWorkBufferLen, wrapperChildWorkBufferSize);
#endif
        if (wrapperReadChildOutputBlock(wrapperChildWorkBuffer + (wrapperChildWorkBufferLen), (int)(wrapperChildWorkBufferSize - wrapperChildWorkBufferLen), &currentBlockRead)) {
            /* Error already reported. */
            return FALSE;
        }

        if (currentBlockRead > 0) {
            /* We read in a block, so increase the length. */
            wrapperChildWorkBufferLen += currentBlockRead;
            if (wrapperChildWorkIsNewLine) {
                wrapperChildWorkLastDataTime = now;
                wrapperChildWorkLastDataTimeMillis = nowMillis;
                wrapperChildWorkIsNewLine = FALSE;
            }
            readThisPass = TRUE;
#ifdef DEBUG_CHILD_OUTPUT
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_INFO, TEXT("  Read %d bytes of new output.  totalBuffLen=%d, buffSize=%d"), currentBlockRead, wrapperChildWorkBufferLen, wrapperChildWorkBufferSize);
#endif
        }

        /* Terminate the string just to avoid errors.  The buffer has an extra character to handle this. */
        wrapperChildWorkBuffer[wrapperChildWorkBufferLen] = '\0';
        
        /* Loop over the contents of the buffer and try and extract as many lines as possible.
         *  Keep track of where we are to avoid unnecessary memory copies.
         *  At this point, the entire buffer will always be unlogged. */
        loggedOffset = 0;
        defer = FALSE;
        while ((wrapperChildWorkBufferLen > loggedOffset) && (!defer)) {
#ifdef DEBUG_CHILD_OUTPUT
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_INFO, TEXT("Inner loop.  totalBuffLen=%d, loggedOffset=%d, unloggedBuffLen=%d, buffSize=%d"), wrapperChildWorkBufferLen, loggedOffset, wrapperChildWorkBufferLen - loggedOffset, wrapperChildWorkBufferSize);
#endif
            /* We have something in the buffer.  Loop and see if we have a complete line to log.
             * We will always find a LF at the end of the line.  On Windows there may be a CR immediately before it. */
            cLF = NULL;
            for (i = loggedOffset; i < wrapperChildWorkBufferLen; i++) {
                /* If there is a null character, replace it with a question mark (\0 is not a termination character in Java). */
                if (wrapperChildWorkBuffer[i] == 0) {
                    wrapperChildWorkBuffer[i] = '?';
                } else if (wrapperChildWorkBuffer[i] == (char)CHAR_LF) {
                    cLF = &wrapperChildWorkBuffer[i];
                    break;
                }
            }
            
            if (cLF != NULL) {
                /* We found a valid LF so we know that a full line is ready to be logged. */
#ifdef WIN32
                if ((cLF > wrapperChildWorkBuffer) && ((cLF - sizeof(char))[0] == 0x0d)) {
 #ifdef DEBUG_CHILD_OUTPUT
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_INFO, TEXT("Found CR+LF"));
 #endif
                    /* Replace the CR with a NULL */
                    (cLF - sizeof(char))[0] = 0;
                } else {
#endif
#ifdef DEBUG_CHILD_OUTPUT
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_INFO, TEXT("Found LF"));
#endif
#ifdef WIN32
                }
#endif
                /* Replace the LF with a NULL */
                cLF[0] = '\0';

                /* We have a string to log. */
#ifdef DEBUG_CHILD_OUTPUT
 #ifdef UNICODE
                /* It is not easy to log the string as is because they are not wide chars. Send it only to stdout. */
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_INFO, TEXT("Log: (see stdout)"));
  #ifdef WIN32
                wprintf(TEXT("Log: [%S]\n"), wrapperChildWorkBuffer + loggedOffset);
  #else
                wprintf(TEXT("Log: [%s]\n"), wrapperChildWorkBuffer + loggedOffset);
  #endif
 #else
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_INFO, TEXT("Log: [%s]"), wrapperChildWorkBuffer + loggedOffset);
 #endif
#endif
                /* Actually log the individual line of output. */
                logChildOutput(wrapperChildWorkBuffer + loggedOffset);
                
                /* Update the offset so we know how far we've logged. */
                loggedOffset = cLF - wrapperChildWorkBuffer + 1;
                wrapperChildWorkIsNewLine = TRUE;
#ifdef DEBUG_CHILD_OUTPUT
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_INFO, TEXT("loggedOffset: %d"), loggedOffset);
#endif
            } else {
                /* If we read this pass or if the last character is a CR on Windows then we always want to defer. */
                if (readThisPass
#ifdef WIN32
                        || (wrapperChildWorkBuffer[wrapperChildWorkBufferLen - 1] == 0x0d)
#endif
                        /* Avoid dumping partial lines because we call this funtion too quickly more than once.
                         *  Never let the line be partial unless more than the LF-Delay threshold has expired. */
                        || (wrapperData->logLFDelayThreshold == 0)
                        || (((now - wrapperChildWorkLastDataTime) * 1000 + (nowMillis - wrapperChildWorkLastDataTimeMillis)) < wrapperData->logLFDelayThreshold)
                    ) {
#ifdef DEBUG_CHILD_OUTPUT
 #ifdef UNICODE
                    /* It is not easy to log the string as is because they are not wide chars. Send it only to stdout. */
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_INFO, TEXT("Incomplete line.  Defer: (see stdout)  Age: %d"),
                        (now - wrapperChildWorkLastDataTime) * 1000 + (nowMillis - wrapperChildWorkLastDataTimeMillis));
  #ifdef WIN32
                    wprintf(TEXT("Defer Log: [%S]\n"), wrapperChildWorkBuffer + loggedOffset);
  #else
                    wprintf(TEXT("Defer Log: [%s]\n"), wrapperChildWorkBuffer + loggedOffset);
  #endif
 #else
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_INFO, TEXT("Incomplete line.  Defer: [%s]  Age: %d"), wrapperChildWorkBuffer,
                        (now - wrapperChildWorkLastDataTime) * 1000 + (nowMillis - wrapperChildWorkLastDataTimeMillis));
 #endif
#endif
                    defer = TRUE;
                } else {
                    /* We have an incomplete line, but it was from a previous pass and is old enough, so we want to log it as it may be a prompt.
                     *  This will always be the complete buffer. */
#ifdef DEBUG_CHILD_OUTPUT
 #ifdef UNICODE
                    /* It is not easy to log the string as is because they are not wide chars. Send it only to stdout. */
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_INFO, TEXT("Incomplete line, but log now: (see stdout)  Age: %d"),
                        (now - wrapperChildWorkLastDataTime) * 1000 + (nowMillis - wrapperChildWorkLastDataTimeMillis));
  #ifdef WIN32
                    wprintf(TEXT("Log: [%S]\n"), wrapperChildWorkBuffer + loggedOffset);
  #else
                    wprintf(TEXT("Log: [%s]\n"), wrapperChildWorkBuffer + loggedOffset);
  #endif
 #else
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_INFO, TEXT("Incomplete line, but log now: [%s]  Age: %d"), wrapperChildWorkBuffer,
                        (now - wrapperChildWorkLastDataTime) * 1000 + (nowMillis - wrapperChildWorkLastDataTimeMillis));
 #endif
#endif
                    logChildOutput(wrapperChildWorkBuffer + loggedOffset);
                    
                    /* We know we read everything so we can safely reset the loggedOffset and clear the buffer. */
                    wrapperChildWorkBuffer[0] = '\0';
                    wrapperChildWorkBufferLen = 0;
                    loggedOffset = 0;
                    wrapperChildWorkIsNewLine = TRUE;
                }
            }
        }
        
        /* We have read as many lines from the buffered output as possible.
         *  If we still have any partial lines, then we need to make sure they are moved to the beginning of the buffer so we can read in another block. */
        if (loggedOffset > 0) {
            if (loggedOffset >= wrapperChildWorkBufferLen) {
                /* We know we have read everything in.  So we can efficiently clear the buffer. */
#ifdef DEBUG_CHILD_OUTPUT
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_INFO, TEXT("Cleared Buffer as everything was logged."));
#endif
                wrapperChildWorkBuffer[0] = '\0';
                wrapperChildWorkBufferLen = 0;
                /* loggedOffset = 0; Not needed. */
            } else {
                /* We have logged one or more lines from the buffer, but unlogged content still exists.  It needs to be moved to the head of the buffer. */
#ifdef DEBUG_CHILD_OUTPUT
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_INFO, TEXT("Moving %d bytes in buffer for next cycle."), wrapperChildWorkBufferLen - loggedOffset);
#endif
                /* NOTE - This line intentionally does the copy within the same memory space.  It is safe the way it is working however. */
                wrapperChildWorkBufferLen = wrapperChildWorkBufferLen - loggedOffset;
                safeMemCpy(wrapperChildWorkBuffer, 0, loggedOffset, wrapperChildWorkBufferLen);
                /* Shouldn't be needed, but just to make sure the buffer has been ended properly */
                wrapperChildWorkBuffer[wrapperChildWorkBufferLen] = 0;
                /* loggedOffset = 0; Not needed. */
            }
        } else {
        }

        if (currentBlockRead <= 0) {
            /* All done for now. */
            if (wrapperChildWorkBufferLen > 0) {
#ifdef DEBUG_CHILD_OUTPUT
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_INFO, TEXT("wrapperReadChildOutput() END (Incomplete)"));
#endif
            } else {
#ifdef DEBUG_CHILD_OUTPUT
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_INFO, TEXT("wrapperReadChildOutput() END"));
#endif
            }
            return FALSE;
        }
        
        /* Get the time again */
        wrapperGetCurrentTime(&timeBuffer);
        now = timeBuffer.time;
        nowMillis = timeBuffer.millitm;
    }

    /* If we got here then we timed out. */
#ifdef DEBUG_CHILD_OUTPUT
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_INFO, TEXT("wrapperReadChildOutput() END TIMEOUT"));
#endif
    return TRUE;
}

void wrapperReadAllChildOutputAfterFailure() {
    /* Set a flag so that there will be no attempt to parse the output. */
    wrapperData->jvmQuerySkipParse = TRUE;
    wrapperData->jvmDefaultLogLevel = LEVEL_ERROR;
    
    /* Read all messages of the output as it may give us a clue of what the problem is. */
    while (wrapperReadChildOutput(250)) { }

    wrapperData->jvmQuerySkipParse = FALSE;
}

/**
 * Immediately after a JVM is launched and whenever the log file name changes,
 *  the log file name is sent to the JVM where it can be referenced by applications.
 */
int sendLogFileName() {
    TCHAR *currentLogFilePath;
    int result = FALSE;

    currentLogFilePath = getCurrentLogfilePath();

    if (!currentLogFilePath) {
        /* try to continue without sending the log file? */
    } else {
        result = wrapperProtocolFunction(WRAPPER_MSG_LOGFILE, currentLogFilePath);
        free(currentLogFilePath);
    }
    return result;
}

#define JVM_TARGET_NONE         0
#define JVM_TARGET_BOOTSTRAP    1
#define JVM_TARGET_DRYRUN       2
#define JVM_TARGET_APP          4
#define JVM_TARGET_DRYRUN_APP   (JVM_TARGET_DRYRUN | JVM_TARGET_APP)
#define JVM_TARGET_ALL          (JVM_TARGET_BOOTSTRAP | JVM_TARGET_DRYRUN | JVM_TARGET_APP)

static int addAppOnlyAdditional(int index) {
    static int appOnlyArrayLen;
    int *appOnlyArrayTemp;
    int i;

    if (!wrapperData->appOnlyAdditionalIndexes) {
        /* Initialize array with size of 10 */
        appOnlyArrayLen = 10;
        wrapperData->appOnlyAdditionalIndexes = malloc(sizeof(int) * appOnlyArrayLen);
        if (!wrapperData->appOnlyAdditionalIndexes) {
            outOfMemory(TEXT("AAOA"), 1);
            return TRUE;
        }
    } else if (appOnlyArrayLen < wrapperData->appOnlyAdditionalCount + 1) {
        /* we need to increase the size of the array */
        appOnlyArrayTemp = wrapperData->appOnlyAdditionalIndexes;
        appOnlyArrayLen = wrapperData->appOnlyAdditionalCount + 10;
        wrapperData->appOnlyAdditionalIndexes = malloc(sizeof(int) * appOnlyArrayLen);
        if (!wrapperData->appOnlyAdditionalIndexes) {
            outOfMemory(TEXT("AAOA"), 2);
            free(appOnlyArrayTemp);
            return TRUE;
        }
        for (i = 0; i < wrapperData->appOnlyAdditionalCount; i++) {
            wrapperData->appOnlyAdditionalIndexes[i] = appOnlyArrayTemp[i];
        }
        free(appOnlyArrayTemp);
    }
    wrapperData->appOnlyAdditionalIndexes[wrapperData->appOnlyAdditionalCount] = index;
    wrapperData->appOnlyAdditionalCount++;

    return FALSE;
}

static int getAdditionalScope(const TCHAR* value) {
    if (value) {
        if (strcmpIgnoreCase(value, TEXT("DRYRUN_APP")) == 0) {
            return JVM_TARGET_DRYRUN_APP;
        } else if (strcmpIgnoreCase(value, TEXT("APP")) == 0) {
            return JVM_TARGET_APP;
        } else if (strcmpIgnoreCase(value, TEXT("BOOTSTRAP_DRYRUN_APP")) == 0) {
            return JVM_TARGET_ALL;
        }
    }
    return JVM_TARGET_NONE;
}

static const TCHAR* getAdditionalScopeStr(int scope) {
    switch (scope) {
        case JVM_TARGET_DRYRUN_APP:
            return TEXT("DRYRUN_APP");
        case JVM_TARGET_APP:
            return TEXT("APP");
        case JVM_TARGET_ALL:
            return TEXT("BOOTSTRAP_DRYRUN_APP");
        case JVM_TARGET_NONE:
            return TEXT("NONE");
        default:
            return TEXT("UNKNOWN");
    }
}
/**
 * This function adjusts the scope of certain JVM options to ensure proper functioning.
 *
 * @param opt      JVM option.
 * @param scope    The current scope.
 * @param propName Name of the scope property if configured by the user, or NULL otherwise.
 * @param warn     TRUE to enable warning when the scope is changed.
 *
 * @return The modified scope.
 */
static int getRealAdditionalScope(const TCHAR* opt, int scope, const TCHAR* propName, int warn) {
    int outScope = scope;

    /* Note: For a long option (starting with '--'), either '--name=value' or '--name value' can be used. */
    if ((_tcsstr(opt, TEXT("--module-path=")) == opt) ||
        (_tcsstr(opt, TEXT("--module-path ")) == opt) ||
        (_tcsstr(opt, TEXT("-p ")) == opt) ||
        (_tcsstr(opt, TEXT("--upgrade-module-path=")) == opt) ||
        (_tcsstr(opt, TEXT("--upgrade-module-path ")) == opt) ||
        (_tcsstr(opt, TEXT("--add-modules=")) == opt) ||
        (_tcsstr(opt, TEXT("--add-modules ")) == opt)) {
        /* These options have already been processed; They are not allowed to
         *  coexist in the configuration with their corresponding properties. */
        outScope = JVM_TARGET_NONE;
        warn = FALSE;
    } else if ((_tcsstr(opt, TEXT("-agentlib:")) == opt) ||
        (_tcsstr(opt, TEXT("-agentpath:")) == opt) ||
        (_tcsstr(opt, TEXT("--class-path=")) == opt) ||
        (_tcsstr(opt, TEXT("--class-path ")) == opt) ||
        (_tcsstr(opt, TEXT("-classpath ")) == opt) ||
        (_tcsstr(opt, TEXT("-cp ")) == opt)) {
        /* TODO: No longer allow classpath option to be used together with wrapper.java.classpath.<n>. */
        if (!propName) {
            /* Only edit if not configured by the user. */
            outScope = JVM_TARGET_ALL;
        }
    } else if ((_tcsstr(opt, TEXT("--describe-module=")) == opt) ||
        (_tcsstr(opt, TEXT("--describe-module ")) == opt) ||
        (_tcsstr(opt, TEXT("-d ")) == opt) ||
        (_tcsstr(opt, TEXT("--list-modules=")) == opt) ||
        (_tcsstr(opt, TEXT("--list-modules ")) == opt) ||
        (_tcsstr(opt, TEXT("--version")) == opt) ||
        (_tcsstr(opt, TEXT("-version")) == opt) ||
        (_tcsstr(opt, TEXT("-Xinternalversion")) == opt) ||
        (_tcsstr(opt, TEXT("--help")) == opt) ||
        (_tcsstr(opt, TEXT("-help")) == opt) ||
        (_tcsstr(opt, TEXT("-h")) == opt) ||
        (_tcsstr(opt, TEXT("-?")) == opt)) {
        /* These options would break the bootstrap call. */
        outScope = (scope & ~JVM_TARGET_BOOTSTRAP);
    } else if ((_tcsstr(opt, TEXT("--dry-run")) == opt)) {
        /* Using --dry-run for the bootstrap call would be bad, so ignore user settings.
         *  Also avoid duplicating this option in the --dry-run call. */
        outScope = (scope & ~JVM_TARGET_BOOTSTRAP & ~JVM_TARGET_DRYRUN);
    }

    if (warn && propName && (scope != outScope)) {
        log_printf(WRAPPER_SOURCE_WRAPPER, properties->logWarningLogLevel, 
            TEXT("The scope for option '%s', configured with property %s=%s, is invalid.  Resolving to %s."),
            opt, propName, getAdditionalScopeStr(scope), getAdditionalScopeStr(outScope));
    }

    return outScope;
}

static size_t getJvmOptionSplitPos(const TCHAR* prop) {
    static TCHAR* splitOptions[] = { TEXT("--add-reads"),     TEXT("--add-exports"),     TEXT("--add-opens"),
                                     TEXT("--limit-modules"), TEXT("--patch-module"),    TEXT("--source"),
                                     TEXT("--class-path"),    TEXT("-classpath"),        TEXT("-cp"),
                                     TEXT("--module-path"),   TEXT("-p"),                TEXT("--upgrade-module-path"),
                                     TEXT("--add-modules"),   TEXT("--describe-module"), TEXT("-d") };
    static int splitOptionsCount = 15;
    int i;
    size_t len;

    for (i = 0; i < splitOptionsCount; i++) {
        if (_tcsstr(prop, splitOptions[i]) == prop) {
            len = _tcslen(splitOptions[i]);
            if (prop[len] == TEXT(' ')) {
                return len;
            } else if (prop[len] == TEXT('=')) {
                break;
            }
        }
    }
    return 0;
}

static int wrapperBuildFileParameters(TCHAR **strings, ParameterFile* parameterFile, int target, int allowSplit, int collectAppOnlyIndexes, int index) {
    int i;
    size_t len;
    size_t splitShift;

    if (parameterFile) {
        for (i = 0; i < parameterFile->paramsCount; i++) {
            if (!(target & parameterFile->scopes[i])) {
                /* Skip this option */
                continue;
            } else if (collectAppOnlyIndexes && !(parameterFile->scopes[i] & JVM_TARGET_DRYRUN)) {
                if (addAppOnlyAdditional(index)) {
                    return -1;
                }
            }

            splitShift = 0;
            if (allowSplit) {
                len = getJvmOptionSplitPos(parameterFile->params[i]);
                if (len > 0) {
                    if (strings) {
                        strings[index] = malloc(sizeof(TCHAR) * (len + 1));
                        if (!strings[index]) {
                            outOfMemory(TEXT("WBFP"), 1);
                            return -1;
                        }
                        _tcsncpy(strings[index], parameterFile->params[i], len);
                        strings[index][len] = 0;
                        splitShift = len + 1;
                        while (parameterFile->params[i][splitShift] == TEXT(' ')) {
                            splitShift++;
                        }
                    }
                    index++;
                }
            }

            if (strings) {
                len = _tcslen(parameterFile->params[i] + splitShift);
                strings[index] = malloc(sizeof(TCHAR) * (len + 1));
                if (!strings[index]) {
                    outOfMemory(TEXT("WBFP"), 2);
                    return -1;
                }
                _sntprintf(strings[index], len + 1, TEXT("%s"), parameterFile->params[i] + splitShift);
            }
            index++;
        }
    }
    return index;
}

/**
 * Builds up the app parameters section of the Java command line.
 *
 * @return The final index into the strings array, or -1 if there were any problems.
 */
int wrapperBuildAppParameterArrayInner(TCHAR **strings, int index, int thisIsTestWrapper, int useBackend) {
    TCHAR *prop;
    TCHAR *prop2;
    int index0 = index;
    int i;
    int isQuotable;
    int copied;
    TCHAR paramBuffer2[128];
    TCHAR **propertyNames;
    TCHAR **propertyValues;
    long unsigned int *propertyIndices;

    if (getStringProperties(properties, TEXT("wrapper.app.parameter."), TEXT(""), wrapperData->ignoreSequenceGaps, FALSE, &propertyNames, &propertyValues, &propertyIndices)) {
        /* Failed */
        return -1;
    }

    i = 0;
    while (propertyNames[i]) {
        prop = propertyValues[i];
        if (_tcslen(prop) >= 0) { /* Note: empty args are allowed. */
            if (thisIsTestWrapper && (i == 1) && ((_tcscmp(prop, TEXT("{{TestWrapperBat}}")) == 0) || (_tcscmp(prop, TEXT("{{TestWrapperSh}}")) == 0))) {
                /* This is the TestWrapper dummy parameter.  Simply skip over it so it doesn't get put into the command line. */
            } else {
                if (strings) {
                    copied = FALSE;
                    _sntprintf(paramBuffer2, 128, TEXT("wrapper.app.parameter.%lu.quotable"), propertyIndices[i]);
                    isQuotable = getBooleanProperty(properties, paramBuffer2, FALSE);
                    if (isQuotable) {
                        prop2 = wrapperUnquoteArg(prop, propertyNames[i], FALSE, FALSE, LEVEL_ERROR);
                        if (!prop2) {
                            /* error reported */
                            freeStringProperties(propertyNames, propertyValues, propertyIndices);
                            return -1;
                        }
                        prop = prop2;
                        copied = TRUE;
                    }
                    if (copied) {
                        strings[index] = prop;
                    } else {
                        updateStringValue(&(strings[index]), prop);
                    }
                    if (!strings[index]) {
                        outOfMemory(TEXT("WBAPAAI"), 2);
                        freeStringProperties(propertyNames, propertyValues, propertyIndices);
                        return -1;
                    }
                }
                index++;
            }
        }
        i++;
    }
    freeStringProperties(propertyNames, propertyValues, propertyIndices);

    /* procede command line parameters */
    if (wrapperData->javaArgValueCount > 0) {
        for (i = 0; i < wrapperData->javaArgValueCount; i++) {
            if (strings) {
                updateStringValue(&(strings[index]), wrapperData->javaArgValues[i]);
                if (!strings[index]) {
                    outOfMemory(TEXT("WBAPAAI"), 3);
                    return -1;
                }
            }
            index++;
        }
    }

    if ((index = wrapperBuildFileParameters(strings, wrapperData->parameterFile, JVM_TARGET_DRYRUN_APP, FALSE, FALSE, index)) < 0) {
        return -1;
    }

    if (useBackend) {
        if ((index - index0) > WRAPPER_APP_PARAMETERS_MAXCOUNT) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Too many application parameters (%d)."), (index - index0));
            return -1;
        }
    } else {
        if (wrapperData->parameterFile && wrapperData->parameterFile->hasCipher) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL,
                TEXT("Parameter file specified with property '%s' contains sensitive data.\n  Passing its parameters via the java command line would be insecure and is therefore not allowed."), TEXT("wrapper.app.parameter_file"));
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL,
                TEXT("  The %s property can be set to TRUE to securely pass parameters to the Java application."), TEXT("wrapper.app.parameter.backend"));
            return -1;
        }
    }

    return index;
}

int wrapperBuildAppPropertyArrayInner(TCHAR **strings) {
    TCHAR *prop;
    TCHAR *prop2;
    int i;
    int isQuotable;
    int copied;
    int index;
    TCHAR **propertyNames;
    TCHAR **propertyValues;
    long unsigned int *propertyIndices;
    TCHAR paramBuffer1[128];

    if (getStringProperties(properties, TEXT("wrapper.app.property."), TEXT(""), wrapperData->ignoreSequenceGaps, FALSE, &propertyNames, &propertyValues, &propertyIndices)) {
        /* Failed */
        return -1;
    }

    i = 0;
    index = 0;
    while (propertyNames[i]) {
        prop = propertyValues[i];
        if (prop) {
            if (_tcslen(prop) > 0) {
                /* All properties must contain a '='. */
                if (!_tcsstr(prop, TEXT("="))) {
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("The value of property '%s' (%s) is not a valid system property."), propertyNames[i], getMaskedValueOfProperty(properties, propertyNames[i]));
                    index = -1;
                    break;
                } else {
                    if (!strings) {
                        if (_tcsncmp(prop, TEXT("-D"), 2) == 0) {
                            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN, TEXT("The value of property '%s' (%s) will not be set on the Java command line.  Using the '-D' prefix is probably incorrect."), propertyNames[i], getMaskedValueOfProperty(properties, propertyNames[i]));
                        }
                    } else {
                        copied = FALSE;

                        _sntprintf(paramBuffer1, 128, TEXT("wrapper.app.property.%lu.quotable"), propertyIndices[i]);
                        isQuotable = getBooleanProperty(properties, paramBuffer1, FALSE);
                        if (isQuotable) {
                            prop2 = wrapperUnquoteArg(prop, propertyNames[i], FALSE, FALSE, LEVEL_ERROR);
                            if (!prop2) {
                                /* error reported */
                                index = -1;
                                break;
                            }
                            prop = prop2;
                            copied = TRUE;
                        }

                        if (copied) {
                            strings[index] = prop;
                        } else {
                            updateStringValue(&(strings[index]), prop);
                        }
                        if (!strings[index]) {
                            outOfMemory(TEXT("WBAPRAI"), 1);
                            index = -1;
                            break;
                        }
                    }
                    index++;
                }
            }
            i++;
        }
    }
    freeStringProperties(propertyNames, propertyValues, propertyIndices);

    if ((index = wrapperBuildFileParameters(strings, wrapperData->propertyFile, JVM_TARGET_DRYRUN_APP, FALSE, FALSE, index)) < 0) {
        return -1;
    }

    if (index > WRAPPER_APP_PROPERTIES_MAXCOUNT) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Too many application properties (%d)."), index);
        return -1;
    }

    return index;
}

/**
 * Immediately after a JVM is launched, application parameters are sent to the JVM (if backend=TRUE is specified).
 */
int sendAppParameters() {
    TCHAR *buffer;
    int result = FALSE;

    if (appParametersLen > 0) {
        buffer = linearizeStringArray(appParameters, appParametersLen, TEXT('\t'), TRUE, TRUE);
        if (!buffer) {
            outOfMemory(TEXT("SAPA"), 1);
            result = TRUE;
        } else {
            result = wrapperProtocolFunction(WRAPPER_MSG_APP_PARAMETERS, buffer);

            wrapperSecureFreeStrW(buffer);
        }
    } else {
        result = wrapperProtocolFunction(WRAPPER_MSG_APP_PARAMETERS, TEXT(""));
    }
    return result;
}

/**
 * Immediately after a JVM is launched, the system properties are sent to the JVM.
 */
int sendAppProperties() {
    TCHAR *buffer;
    int result = FALSE;

    if (appPropertiesLen > 0) {
        buffer = linearizeStringArray(appProperties, appPropertiesLen, TEXT('\t'), TRUE, FALSE);
        if (!buffer) {
            outOfMemory(TEXT("SAPR"), 1);
            result = TRUE;
        } else {
            result = wrapperProtocolFunction(WRAPPER_MSG_APP_PROPERTIES, buffer);

            wrapperSecureFreeStrW(buffer);
        }
    } else {
        result = wrapperProtocolFunction(WRAPPER_MSG_APP_PROPERTIES, TEXT(""));
    }
    return result;
}

/**
 * Immediately after a JVM is launched, the wrapper configuration is sent to the
 *  JVM where it can be used as a properties object.
 */
int sendProperties() {
    TCHAR* buffer;
    int result = FALSE;

    if (properties != initialProperties) {
        adjustStickyProperties(properties, initialProperties, FALSE);
    }

    buffer = linearizeProperties(properties, TEXT('\t'), !(wrapperData->shareAllConfiguration), TRUE);

    if (!buffer) {
        result = TRUE;
    } else {
        result = wrapperProtocolFunction(WRAPPER_MSG_PROPERTIES, buffer);

        free(buffer);
    }
    return result;
}

void resetJavaPid() {
    wrapperData->javaPID = 0;
#ifndef WIN32
    wrapperData->javaPidUpdateStatus = FALSE;
    wrapperData->javaPidWaitStatus = 0;
#endif
}

/**
 * Common cleanup code which should get called when we first decide that the JVM was down.
 */
void wrapperJVMDownCleanup(int setState) {
    /* Only set the state to DOWN_CHECK if we are not already in a state which reflects this. */
    if (setState) {
        if (wrapperData->jvmCleanupTimeout > 0) {
            wrapperSetJavaState(WRAPPER_JSTATE_DOWN_CHECK, wrapperGetTicks(), wrapperData->jvmCleanupTimeout);
        } else {
            wrapperSetJavaState(WRAPPER_JSTATE_DOWN_CHECK, wrapperGetTicks(), -1);
        }
    }

    /* Remove java pid file if it was registered and created by this process. */
    if (wrapperData->javaPidFilename) {
        _tunlink(wrapperData->javaPidFilename);
    }

    /* Reset the Java PID.
     *  Also do it in wrapperJVMProcessExited(), whichever is called first. */
    resetJavaPid();

#ifdef WIN32
    if (wrapperData->javaProcess) {
        if (!CloseHandle(wrapperData->javaProcess)) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR,
                TEXT("Failed to close the Java process handle: %s"), getLastErrorText());
        }
        wrapperData->javaProcess = NULL;
    }
#endif

    /* Close any open socket to the JVM */
    if (wrapperData->stoppedPacketReceived) {
        wrapperProtocolClose();
    } else {
        /* Leave the socket open so the Wrapper has the chance to read any outstanding packets. */
    }
}

/**
 * Attempt to immediately kill the JVM process.
 */
int wrapperKillProcessNow() {
    int ret;
    int error;

    /* Make sure this flag is initiated to FALSE when calling this function. */
    wrapperData->jvmTerminatedBeforeKill = FALSE;

    /* Check to make sure that the JVM process is still running (note: On Unix, this function calls waitpid()) */
    ret = wrapperQuickCheckJavaProcessStatus();
    if (ret != WRAPPER_PROCESS_DOWN) {
        if (ret == WRAPPER_PROCESS_UNKNOWN) {
            /* Still attempt to terminate the process (otherwise we can't continue). */
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Unable to determine process status before killing the JVM (0x%x)"), getLastError());
        }

        /* JVM is still up when it should have already stopped itself. */

        /* The JVM process is not responding so the only choice we have is to
         *  kill it. */
#ifdef WIN32
        /* The TerminateProcess funtion will kill the process, but it
         *  does not correctly notify the process's DLLs that it is shutting
         *  down.  Ideally, we would call ExitProcess, but that can only be
         *  called from within the process being killed. */
        if (TerminateProcess(wrapperData->javaProcess, 0)) {
#else
        if (kill(wrapperData->javaPID, SIGKILL) == 0) { 
#endif
            if (!wrapperData->jvmSilentKill) {
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT("JVM did not exit on request, termination requested."));
            }
            return FALSE;
        } else {
            error = getLastError();
#ifdef WIN32
            if (error == ERROR_ACCESS_DENIED) {
                /* The JVM termination already started before we tried killing it, but depending on its memory
                 *  (or the system memory) load, there may be a short delay until the process actually completes.
                 *  Or the JVM might already be gone. In both cases an ERROR_ACCESS_DENIED is returned.
                 *  Simply go to the next state (WRAPPER_JSTATE_KILLED) which will confirm the status of the process. */
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Kill signal sent to the JVM, but it was already terminating."));
                wrapperData->jvmTerminatedBeforeKill = TRUE;
                return FALSE;
            }
#else
            if (error == ESRCH) {
                /* There is a tiny chance that the JVM was gone between the time we tested it and our attempt to kill it.
                 *  If the JVM was already killed but not waited (<defunct>), kill() would return 0, so we know that
                 *  ESRCH really means the process is terminated. */
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Kill signal sent to the JVM, but it was already terminated."));
                wrapperData->jvmTerminatedBeforeKill = TRUE;
                return FALSE;
            }
#endif
            if (!wrapperData->jvmSilentKill) {
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT("JVM did not exit on request."));
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT("  Attempt to terminate process failed: %s"), getErrorText(error, NULL));
            } else {
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT("Attempt to terminate the JVM failed: %s"), getLastErrorText());
            }
            /* Terminating the current JVM failed. Cancel pending restart requests */
            wrapperJVMDownCleanup(TRUE);
            wrapperData->exitCode = wrapperData->errorExitCode;
            return TRUE;
        }
    } else {
        /* The process is gone. A log message will be logged later. */
        wrapperData->jvmTerminatedBeforeKill = TRUE;
    }
    return FALSE;
}

/**
 * Puts the Wrapper into a state where the JVM will be killed at the soonest
 *  possible opportunity.  It is necessary to wait a moment if a final thread
 *  dump is to be requested.  This call will always set the JVM state to
 *  WRAPPER_JSTATE_KILLING.
 *
 * @param silent TRUE to skip messages saying that the JVM did not exit on request.
 *               This is useful in certain cases where we kill the JVM without trying
 *               to shut it down cleanly.
 */
void wrapperKillProcess(int silent) {
#ifdef WIN32
    int ret;
#endif
    int delay = 0;

    if ((wrapperData->jState == WRAPPER_JSTATE_DOWN_CLEAN) ||
        (wrapperData->jState == WRAPPER_JSTATE_LAUNCH_DELAY) ||
        (wrapperData->jState == WRAPPER_JSTATE_DOWN_CHECK) ||
        (wrapperData->jState == WRAPPER_JSTATE_DOWN_FLUSH_STDIN) ||
        (wrapperData->jState == WRAPPER_JSTATE_DOWN_FLUSH)) {
        /* Already down. */
        if (wrapperData->jState == WRAPPER_JSTATE_LAUNCH_DELAY) {
            wrapperSetJavaState(WRAPPER_JSTATE_DOWN_CLEAN, wrapperGetTicks(), 0);
        }
        return;
    }

    /* Check to make sure that the JVM process is still running */
#ifdef WIN32
    ret = WaitForSingleObject(wrapperData->javaProcess, 0);
    if (ret == WAIT_TIMEOUT) {
#else
    if (waitpid(wrapperData->javaPID, NULL, WNOHANG) == 0) {
#endif
        /* JVM is still up when it should have already stopped itself. */
        if (!silent) {
            /* Only request a thread dump if we requested the JVM to shutdown cleanly. */
            if (wrapperData->requestThreadDumpOnFailedJVMExit) {
                wrapperRequestDumpJVMState();

                delay = wrapperData->requestThreadDumpOnFailedJVMExitDelay;
            }
        }
    }

    wrapperSetJavaState(WRAPPER_JSTATE_KILLING, wrapperGetTicks(), delay);
    wrapperData->jvmSilentKill = silent;
}


/**
 * Add some checks of the properties to try to catch the case where the user is making use of TestWrapper scripts.
 *
 * @return TRUE if there is such a missconfiguration.  FALSE if all is Ok.
 */
int checkWrapperScript() {
    const TCHAR* prop;
    TCHAR* var;

#ifdef WIN32
    var = _tgetenv(TEXT("_WRAPPER_SCRIPT_VERSION"));
#else
    var = _tgetenv(TEXT("WRAPPER_SCRIPT_VERSION"));
#endif
    if (var) {
        if (_tcscmp(wrapperVersionRoot, var) != 0) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN, TEXT("The version of the script (%s) doesn't match the version of this Wrapper (%s). This might cause some problems"), var, wrapperVersionRoot);
        }
#if !defined(WIN32) && defined(UNICODE)
        free(var);
#endif
    }

    prop = getStringProperty(properties, TEXT("wrapper.java.mainclass"), NULL);
    if (prop) {
        if (_tcscmp(prop, TEXT("org.tanukisoftware.wrapper.test.Main")) == 0) {
            /* This is the TestWrapper app.  So don't check. */
        } else {
            /* This is a user application, so make sure that they are not using the TestWrapper scripts. */
#ifdef WIN32
            var = _tgetenv(TEXT("_WRAPPER_SCRIPT_NAME"));
#else
            var = _tgetenv(TEXT("WRAPPER_SCRIPT_NAME"));
#endif
            if (var) {
                if (_tcscmp(var, TEXT("testwrapper")) == 0) {
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT(
                        ""));
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT(
                        "--------------------------------------------------------------------"));
#ifdef WIN32
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT(
                        "We have detected that you are making use of the sample batch files\nthat are designed for the TestWrapper Example Application.  When\nsetting up your own application, please copy fresh files over from\nthe Wrapper's src\\bin directory."));
#else
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT(
                        "We have detected that you are making use of the sample shell scripts\nthat are designed for the TestWrapper Example Application.  When\nsetting up your own application, please copy fresh files over from\nthe Wrapper's src/bin directory."));
#endif
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT(
                        ""));
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT(
                        "Shutting down as this will likely cause problems with your\napplication startup."));
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT(
                        ""));
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT(
                        "Please see the integration section of the documentation for more\ninformation."));
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT(
                        "  https://wrapper.tanukisoftware.com/integrate"));
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT(
                        "--------------------------------------------------------------------"));
                    return TRUE;
                }
#if !defined(WIN32) && defined(UNICODE)
                free(var);
#endif
            }
        }
    }
    return FALSE;
}

#ifdef WIN32

/**
 * Creates a human readable representation of the Windows OS the Wrapper is run on.
 *
 * @param pszOS the buffer the information gets stored to
 * @return FALSE if error or no information could be retrieved. TRUE otherwise.
 */
BOOL GetOSDisplayString(TCHAR** pszOS) {
    OSVERSIONINFOEX osvi;
    SYSTEM_INFO si;
    FARPROC pGNSI;
    FARPROC pGPI;
    DWORD dwType;
    TCHAR buf[80];

    ZeroMemory(&si, sizeof(SYSTEM_INFO));
    ZeroMemory(&osvi, sizeof(OSVERSIONINFOEX));

    osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEX);

#pragma warning(push)
#pragma warning(disable : 4996) /* Visual Studio 2013 deprecates GetVersionEx but we still want to use it. */
    if (!GetVersionEx((OSVERSIONINFO*) &osvi)) {
         return FALSE;
    }
#pragma warning(pop)

    /* Call GetNativeSystemInfo if supported or GetSystemInfo otherwise.*/

    pGNSI = GetProcAddress(GetModuleHandle(TEXT("kernel32.dll")), "GetNativeSystemInfo");
    if (NULL != pGNSI) {
        pGNSI(&si);
    } else {
        GetSystemInfo(&si);
    }

    if ((VER_PLATFORM_WIN32_NT == osvi.dwPlatformId) && (osvi.dwMajorVersion > 4)) {
        _tcsncpy(*pszOS, TEXT("Microsoft "), OSBUFSIZE);

        /* Test for the specific product. */
        if (osvi.dwMajorVersion == 10) {
            if (osvi.dwMinorVersion == 0 ) {
                if (osvi.wProductType == VER_NT_WORKSTATION) {
                    if (osvi.dwBuildNumber >= 22000) {
                        _tcsncat(*pszOS, TEXT("Windows 11 "), OSBUFSIZE);
                    } else {
                        _tcsncat(*pszOS, TEXT("Windows 10 "), OSBUFSIZE);
                    }
                } else {
                    if (osvi.dwBuildNumber >= 26100) {
                        _tcsncat(*pszOS, TEXT("Windows Server 2025 "), OSBUFSIZE);
                    } else if (osvi.dwBuildNumber >= 20348) {
                        _tcsncat(*pszOS, TEXT("Windows Server 2022 "), OSBUFSIZE);
                    } else if (osvi.dwBuildNumber >= 17763) {
                        _tcsncat(*pszOS, TEXT("Windows Server 2019 "), OSBUFSIZE);
                    } else {
                        _tcsncat(*pszOS, TEXT("Windows Server 2016 "), OSBUFSIZE);
                    }
                }
            }
        } else if (osvi.dwMajorVersion == 6) {
            if (osvi.dwMinorVersion == 0 ) {
                if (osvi.wProductType == VER_NT_WORKSTATION) {
                    _tcsncat(*pszOS, TEXT("Windows Vista "), OSBUFSIZE);
                } else {
                    _tcsncat(*pszOS, TEXT("Windows Server 2008 "), OSBUFSIZE);
                }
            } else if (osvi.dwMinorVersion == 1) {
                if (osvi.wProductType == VER_NT_WORKSTATION) {
                    _tcsncat(*pszOS, TEXT("Windows 7 "), OSBUFSIZE);
                } else {
                    _tcsncat(*pszOS, TEXT("Windows Server 2008 R2 "), OSBUFSIZE);
                }
            } else if ( osvi.dwMinorVersion == 2 ) {
                if( osvi.wProductType == VER_NT_WORKSTATION ) {
                    _tcsncat(*pszOS, TEXT("Windows 8 "), OSBUFSIZE);
                } else {
                    _tcsncat(*pszOS, TEXT("Windows Server 2012 "), OSBUFSIZE);
                }
            } else if ( osvi.dwMinorVersion == 3 ) {
                if( osvi.wProductType == VER_NT_WORKSTATION ) {
                    _tcsncat(*pszOS, TEXT("Windows 8.1 "), OSBUFSIZE);
                } else {
                    _tcsncat(*pszOS, TEXT("Windows Server 2012 R2 "), OSBUFSIZE);
                }
            }
        } else if ((osvi.dwMajorVersion == 5) && (osvi.dwMinorVersion == 2)) {
            if (GetSystemMetrics(89)) {
                _tcsncat(*pszOS, TEXT("Windows Server 2003 R2, "), OSBUFSIZE);
            } else if (osvi.wSuiteMask & 8192) {
                _tcsncat(*pszOS, TEXT("Windows Storage Server 2003"), OSBUFSIZE);
            } else if (osvi.wSuiteMask & 32768) {
                _tcsncat(*pszOS, TEXT("Windows Home Server"), OSBUFSIZE);
            } else if (osvi.wProductType == VER_NT_WORKSTATION && si.wProcessorArchitecture==PROCESSOR_ARCHITECTURE_AMD64) {
                _tcsncat(*pszOS, TEXT("Windows XP Professional x64 Edition"), OSBUFSIZE);
            } else {
                _tcsncat(*pszOS, TEXT("Windows Server 2003, "), OSBUFSIZE);
            }

            /* Test for the server type. */
            if (osvi.wProductType != VER_NT_WORKSTATION) {
                if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_IA64) {
                    if (osvi.wSuiteMask & VER_SUITE_DATACENTER) {
                        _tcsncat(*pszOS, TEXT("Datacenter Edition for Itanium-based Systems"), OSBUFSIZE);
                    } else if (osvi.wSuiteMask & VER_SUITE_ENTERPRISE) {
                        _tcsncat(*pszOS, TEXT("Enterprise Edition for Itanium-based Systems"), OSBUFSIZE);
                    }
                } else if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64) {
                    if (osvi.wSuiteMask & VER_SUITE_DATACENTER) {
                        _tcsncat(*pszOS, TEXT("Datacenter x64 Edition"), OSBUFSIZE);
                    } else if (osvi.wSuiteMask & VER_SUITE_ENTERPRISE) {
                        _tcsncat(*pszOS, TEXT("Enterprise x64 Edition"), OSBUFSIZE);
                    } else {
                        _tcsncat(*pszOS, TEXT("Standard x64 Edition"), OSBUFSIZE);
                    }
                } else {
                    if (osvi.wSuiteMask & VER_SUITE_COMPUTE_SERVER) {
                        _tcsncat(*pszOS, TEXT("Compute Cluster Edition"), OSBUFSIZE);
                    } else if (osvi.wSuiteMask & VER_SUITE_DATACENTER) {
                        _tcsncat(*pszOS, TEXT("Datacenter Edition"), OSBUFSIZE);
                    } else if (osvi.wSuiteMask & VER_SUITE_ENTERPRISE) {
                        _tcsncat(*pszOS, TEXT("Enterprise Edition"), OSBUFSIZE);
                    } else if (osvi.wSuiteMask & VER_SUITE_BLADE) {
                        _tcsncat(*pszOS, TEXT("Web Edition" ), OSBUFSIZE);
                    } else {
                        _tcsncat(*pszOS, TEXT("Standard Edition"), OSBUFSIZE);
                    }
                }
            }
        } else if ((osvi.dwMajorVersion == 5) && (osvi.dwMinorVersion == 1)) {
            _tcsncat(*pszOS, TEXT("Windows XP "), OSBUFSIZE);
            if (osvi.wSuiteMask & VER_SUITE_PERSONAL) {
                _tcsncat(*pszOS, TEXT("Home Edition"), OSBUFSIZE);
            } else {
                _tcsncat(*pszOS, TEXT("Professional"), OSBUFSIZE);
            }
        } else if ((osvi.dwMajorVersion == 5) && (osvi.dwMinorVersion == 0)) {
            _tcsncat(*pszOS, TEXT("Windows 2000 "), OSBUFSIZE);
            if (osvi.wProductType == VER_NT_WORKSTATION) {
                _tcsncat(*pszOS, TEXT("Professional"), OSBUFSIZE);
            } else {
                if (osvi.wSuiteMask & VER_SUITE_DATACENTER) {
                    _tcsncat(*pszOS, TEXT("Datacenter Server"), OSBUFSIZE);
                } else if (osvi.wSuiteMask & VER_SUITE_ENTERPRISE) {
                    _tcsncat(*pszOS, TEXT("Advanced Server"), OSBUFSIZE);
                } else {
                    _tcsncat(*pszOS, TEXT("Server"), OSBUFSIZE);
                }
            }
        }

        if (osvi.dwMajorVersion >= 6) {
            pGPI = GetProcAddress(GetModuleHandle(TEXT("kernel32.dll")), "GetProductInfo");

            pGPI(osvi.dwMajorVersion, osvi.dwMinorVersion, 0, 0, &dwType);

            switch (dwType) {
                case 1:
                    _tcsncat(*pszOS, TEXT("Ultimate Edition" ), OSBUFSIZE);
                    break;
                case 48:
                    _tcsncat(*pszOS, TEXT("Professional"), OSBUFSIZE);
                    break;
                case 3:
                    _tcsncat(*pszOS, TEXT("Home Premium Edition"), OSBUFSIZE);
                    break;
                case 67:
                    _tcsncat(*pszOS, TEXT("Home Basic Edition"), OSBUFSIZE);
                    break;
                case 4:
                    _tcsncat(*pszOS, TEXT("Enterprise Edition"), OSBUFSIZE);
                    break;
                case 6:
                    _tcsncat(*pszOS, TEXT("Business Edition"), OSBUFSIZE);
                    break;
                case 11:
                    _tcsncat(*pszOS, TEXT("Starter Edition"), OSBUFSIZE);
                    break;
                case 18:
                    _tcsncat(*pszOS, TEXT("Cluster Server Edition"), OSBUFSIZE);
                    break;
                case 8:
                    _tcsncat(*pszOS, TEXT("Datacenter Edition"), OSBUFSIZE);
                    break;
                case 12:
                    _tcsncat(*pszOS, TEXT("Datacenter Edition (core installation)"), OSBUFSIZE);
                    break;
                case 10:
                    _tcsncat(*pszOS, TEXT("Enterprise Edition"), OSBUFSIZE);
                    break;
                case 14:
                    _tcsncat(*pszOS, TEXT("Enterprise Edition (core installation)"), OSBUFSIZE);
                    break;
                case 15:
                    _tcsncat(*pszOS, TEXT("Enterprise Edition for Itanium-based Systems"), OSBUFSIZE);
                    break;
                case 9:
                    _tcsncat(*pszOS, TEXT("Small Business Server"), OSBUFSIZE);
                    break;
                case 25:
                    _tcsncat(*pszOS, TEXT("Small Business Server Premium Edition"), OSBUFSIZE);
                    break;
                case 7:
                    _tcsncat(*pszOS, TEXT("Standard Edition"), OSBUFSIZE);
                    break;
                case 13:
                    _tcsncat(*pszOS, TEXT("Standard Edition (core installation)"), OSBUFSIZE);
                    break;
                case 17:
                    _tcsncat(*pszOS, TEXT("Web Server Edition"), OSBUFSIZE);
                    break;
                case 101:
                    _tcsncat(*pszOS, TEXT("Home"), OSBUFSIZE);
                    break;
                case 98:
                    _tcsncat(*pszOS, TEXT("Home N"), OSBUFSIZE);
                    break;
                case 99:
                    _tcsncat(*pszOS, TEXT("Home China"), OSBUFSIZE);
                    break;
                case 100:
                    _tcsncat(*pszOS, TEXT("Home Single Language"), OSBUFSIZE);
                    break;
                case 104:
                    _tcsncat(*pszOS, TEXT("Mobile"), OSBUFSIZE);
                    break;
                case 133:
                    _tcsncat(*pszOS, TEXT("Mobile Enterprise"), OSBUFSIZE);
                    break;
                case 121:
                    _tcsncat(*pszOS, TEXT("Education"), OSBUFSIZE);
                    break;
                case 122:
                    _tcsncat(*pszOS, TEXT("Education N"), OSBUFSIZE);
                    break;
                case 70:
                    _tcsncat(*pszOS, TEXT("Enterprise E"), OSBUFSIZE);
                    break;
                case 84:
                    _tcsncat(*pszOS, TEXT("Enterprise N (evaluation installation)"), OSBUFSIZE);
                    break;
                case 27:
                    _tcsncat(*pszOS, TEXT("Enterprise N"), OSBUFSIZE);
                    break;
                case 72:
                    _tcsncat(*pszOS, TEXT("Enterprise (evaluation installation)"), OSBUFSIZE);
                    break;
            }
        }

        /* Include service pack (if any) and build number. */
        if (_tcslen(osvi.szCSDVersion) > 0) {
            _tcsncat(*pszOS, TEXT(" "), OSBUFSIZE);
            _tcsncat(*pszOS, osvi.szCSDVersion, OSBUFSIZE);
        }
        _sntprintf(buf, 80, TEXT(" (build %d)"), osvi.dwBuildNumber);
        _tcsncat(*pszOS, buf, OSBUFSIZE);

        if (osvi.dwMajorVersion >= 6) {
            if ((si.wProcessorArchitecture & PROCESSOR_ARCHITECTURE_IA64) || (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64)) {
                _tcsncat(*pszOS, TEXT(", 64-bit"), OSBUFSIZE);
            } else if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL) {
                _tcsncat(*pszOS, TEXT(", 32-bit"), OSBUFSIZE);
            }
        }
        return TRUE;
    } else {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Unknown Windows Version"));
        return FALSE;
    }
}
#endif

#ifndef ENABLE_QUICK_EDIT_MODE
 #define ENABLE_QUICK_EDIT_MODE  0x0040
#endif

/**
 * Launch common setup code.
 */
int wrapperRunCommonInner() {
    const TCHAR *prop;
#ifdef WIN32
    TCHAR* szOS;
    DWORD consoleMode;
    HANDLE consoleHandle;
    int quickEditStatus;
#endif
    struct tm timeTM;
    TCHAR* tz1;
    TCHAR* tz2;
#if defined(UNICODE)
    size_t req;
#endif

    /* Make sure the tick timer is working correctly. */
    if (wrapperTickAssertions()) {
        return 1;
    }

    /* Log a startup banner. */
    wrapperVersionBanner();

    /* The following code will display a licensed to block if a license key is found
     *  in the Wrapper configuration.  This piece of code is required as is for
     *  Development License owners to be in complience with their development license.
     *  This code does not do any validation of the license keys and works differently
     *  from the license code found in the Standard and Professional Editions of the
     *  Wrapper. */
    prop = getStringProperty(properties, TEXT("wrapper.license.type"), TEXT(""));
    if (strcmpIgnoreCase(prop, TEXT("DEV")) == 0) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS,
            TEXT("  Licensed to %s for %s"),
            getStringProperty(properties, TEXT("wrapper.license.licensee"), TEXT("(LICENSE INVALID)")),
            getStringProperty(properties, TEXT("wrapper.license.dev_application"), TEXT("(LICENSE INVALID)")));
    }
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT(""));

    if (checkWrapperScript()) {
        return 1;
    }

#ifdef WIN32
    if (initializeStartup()) {
        return 1;
    }
    
    if (wrapperProcessHasVisibleConsole()) {
        consoleHandle = GetStdHandle(STD_INPUT_HANDLE);
        if (consoleHandle == NULL) {
            /* Requested handle does not exist. */
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_INFO, TEXT("Standard input does not exist for the process."));
        } else if (consoleHandle == INVALID_HANDLE_VALUE) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT("Faild to retrieve the standard input handle: %s"), getLastErrorText());
        } else {
            if (!GetConsoleMode(consoleHandle, &consoleMode)) {
                if (GetLastError() == ERROR_INVALID_HANDLE) {
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("The standard input appears to be a pipe.  Not checking console mode."));
                } else {
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT("Failed to retrieve the current input mode of the console: %s"), getLastErrorText());
                }
            } else {
                quickEditStatus = getStatusProperty(properties, TEXT("wrapper.console.quickedit"), STATUS_DISABLED);
                if (consoleMode & ENABLE_QUICK_EDIT_MODE) {
                    /* Quick mode was enabled. */
                    if (quickEditStatus == STATUS_DISABLED) {
                        /* Disable quick edit mode. */
                        if (!SetConsoleMode(consoleHandle, consoleMode & ~ENABLE_QUICK_EDIT_MODE)) {
                            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT("Failed to disable QuickEdit Mode for the console: %s"), getLastErrorText());
                            quickEditStatus = STATUS_ENABLED;
                        }
                    } else if (quickEditStatus == STATUS_UNCHANGED) {
                        quickEditStatus = STATUS_ENABLED;
                    }
                } else {
                    /* Quick mode was disabled. */
                    if (quickEditStatus == STATUS_ENABLED) {
                        /* Enable quick edit mode. */
                        if (!SetConsoleMode(consoleHandle, consoleMode | ENABLE_QUICK_EDIT_MODE)) {
                            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT("Failed to enable QuickEdit Mode for the console: %s"), getLastErrorText());
                            quickEditStatus = STATUS_DISABLED;
                        }
                    }
                }
                if (quickEditStatus == STATUS_ENABLED) {
                    log_printf(WRAPPER_SOURCE_WRAPPER, getLogLevelForName(getStringProperty(properties, TEXT("wrapper.console.quickedit.loglevel"), TEXT("WARN"))),
                            TEXT("Running in a console with QuickEdit Mode. Be careful when selecting text in the\n  console as this may cause to block the Java Application.\n"));
                }
            }
        }
    }
#endif

    if (wrapperData->isDebugging) {
        timeTM = wrapperGetReleaseTime();
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Release time: %04d/%02d/%02d %02d:%02d:%02d"),
                timeTM.tm_year + 1900, timeTM.tm_mon + 1, timeTM.tm_mday,
                timeTM.tm_hour, timeTM.tm_min, timeTM.tm_sec );

        timeTM = wrapperGetBuildTime();
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Build time:   %04d/%02d/%02d %02d:%02d:%02d"),
                timeTM.tm_year + 1900, timeTM.tm_mon + 1, timeTM.tm_mday,
                timeTM.tm_hour, timeTM.tm_min, timeTM.tm_sec );

        /* Display timezone information. */
        tzset();
#if defined(UNICODE)
 #if !defined(WIN32)
        req = mbstowcs(NULL, tzname[0], MBSTOWCS_QUERY_LENGTH);
        if (req == (size_t)-1) {
            return 1;
        }
        tz1 = malloc(sizeof(TCHAR) * (req + 1));
        if (!tz1) {
            outOfMemory(TEXT("LHN"), 1);
        } else {
            mbstowcs(tz1, tzname[0], req + 1);
            tz1[req] = TEXT('\0'); /* Avoid bufferflows caused by badly encoded characters. */
            
            req = mbstowcs(NULL, tzname[1], MBSTOWCS_QUERY_LENGTH);
            if (req == (size_t)-1) {
                free(tz1);
                return 1;
            }
            tz2 = malloc(sizeof(TCHAR) * (req + 1));
            if (!tz2) {
                outOfMemory(TEXT("LHN"), 2);
                free(tz1);
            } else {
                mbstowcs(tz2, tzname[1], req + 1);
                tz2[req] = TEXT('\0'); /* Avoid bufferflows caused by badly encoded characters. */
 #else
        req = MultiByteToWideChar(CP_OEMCP, 0, tzname[0], -1, NULL, 0);
        if (req <= 0) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN,
                TEXT("Invalid multibyte sequence in port address \"%s\" : %s"), tzname[0], getLastErrorText());
            return 1;
        }

        tz1 = malloc((req + 1) * sizeof(TCHAR));
        if (!tz1) {
            outOfMemory(TEXT("LHN"), 1);
        } else {
            MultiByteToWideChar(CP_OEMCP,0, tzname[0], -1, tz1, (int)req + 1);
            req = MultiByteToWideChar(CP_OEMCP, 0, tzname[1], -1, NULL, 0);
            if (req <= 0) {
                free(tz1);
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN,
                    TEXT("Invalid multibyte sequence in port address \"%s\" : %s"), tzname[1], getLastErrorText());
                return 1;
            }
            tz2 = malloc((req  + 1) * sizeof(TCHAR));
            if (!tz2) {
                free(tz1);
                outOfMemory(TEXT("LHN"), 2);
            } else {
                MultiByteToWideChar(CP_OEMCP,0, tzname[1], -1, tz2, (int)req + 1);
 #endif

#else
        tz1 = tzname[0];
        tz2 = tzname[1];
#endif
#ifndef FREEBSD
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Timezone:     %s (%s) Offset: %ld, hasDaylight: %d"),
                        tz1, tz2, timezone, daylight);
#else
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Timezone:     %s (%s) Offset: %ld"),
                        tz1, tz2, timezone);
#endif
                if (wrapperData->useSystemTime) {
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Using system timer."));
                } else {
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Using tick timer."));
                }
#ifdef UNICODE
                free(tz1);
                free(tz2);
            }
        }
#endif
        
        /* Log the Wrapper's PID. */
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("PID:          %d"), wrapperData->wrapperPID);
    }

#ifdef WIN32
    if (wrapperData->isDebugging) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Current User: %s  Domain: %s"), (wrapperData->userName ? wrapperData->userName : TEXT("N/A")), (wrapperData->domainName ? wrapperData->domainName : TEXT("N/A")));
        szOS = calloc(OSBUFSIZE, sizeof(TCHAR));
        if (szOS) {
            if (GetOSDisplayString(&szOS)) {
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Operating System ID: %s"), szOS);
            }
            free(szOS);
        }
        
        if (isCygwin()) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Cygwin detected"));
        }
        
        if (_tcscmp(wrapperBits, TEXT("32")) == 0) {
            if (wrapperData->DEPStatus) {
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("DEP status: Enabled"));
            } else if (!wrapperData->DEPApiAvailable) {
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("DEP status: Not supported"));
            } else if (wrapperData->DEPError == 5) {
                /* If the operating system is configured to always use DEP for all processes,
                 *  then SetProcessDEPPolicy() will return an Access Denied Error (5), but 
                 *  that doesn't mean DEP is Disabled.*/
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("DEP status: Unchanged (set by the OS)"));
            } else if (wrapperData->DEPError) {
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("DEP status: Disabled (0x%x)"), wrapperData->DEPError);
            } else {
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("DEP status: Disabled"));
            }
        }
    }
#endif

#ifdef FREEBSD
    /* log the iconv library in use. */
    if (wrapperData->isDebugging) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Iconv library: %s"), getIconvLibName());
    }
#endif

    /* Dump the configured properties */
    dumpProperties(properties);

    /* Dump the environment variables */
    dumpEnvironment();
    
#ifndef WIN32
    showResourceslimits();
#endif

#ifdef _DEBUG
    /* Multi-line logging tests. */
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("----- Should be 5 lines -----"));
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("\nLINE2:\n\nLINE4:\n"));
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("----- Next is one line ------"));
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT(""));
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("----- Next is two lines -----"));
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("\n"));
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("----- Next is two lines -----"));
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("ABC\nDEF"));
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("-----------------------------"));
#endif

#ifdef WRAPPER_FILE_DEBUG
    wrapperFileTests();
#endif

    return 0;
}

int wrapperRunCommon(const TCHAR *runMode) {
    int exitCode;

    /* Setup the wrapperData structure. */
    wrapperSetWrapperState(WRAPPER_WSTATE_STARTING);
    wrapperSetJavaState(WRAPPER_JSTATE_DOWN_CLEAN, 0, -1);

    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("--> Wrapper Started as %s"), runMode);
    wrapperData->runCommonStarted = TRUE;

    /* Initialize the wrapper */
    exitCode = wrapperInitializeRun();
    if (exitCode == 0) {
        if (!wrapperRunCommonInner()) {
            /* Enter main event loop */
            wrapperEventLoop();
            
            /* Clean up any open sockets. */
            wrapperProtocolClose();
            protocolStopServer();
            
            exitCode = wrapperData->exitCode;
        } else {
            exitCode = wrapperData->errorExitCode;
        }
    } else {
        exitCode = wrapperData->errorExitCode;
    }

    return exitCode;
}

/**
 * Launch the wrapper as a console application.
 */
int wrapperRunConsole() {
    return wrapperRunCommon(TEXT("Console"));
}

/**
 * Launch the wrapper as a service application.
 */
int wrapperRunService() {
    return wrapperRunCommon(
#ifdef WIN32
        TEXT("Service")
#else
        TEXT("Daemon")
#endif
        );
}

/**
 * Used to ask the state engine to shut down the JVM and Wrapper.
 *
 * @param exitCode Exit code to use when shutting down.
 * @param force True to force the Wrapper to shutdown even if some configuration
 *              had previously asked that the JVM be restarted.  This will reset
 *              any existing restart requests, but it will still be possible for
 *              later actions to request a restart.
 */
void wrapperStopProcess(int exitCode, int force) {
    /* If we are pausing or paused, cancel it. */
    if ((wrapperData->wState == WRAPPER_WSTATE_PAUSING) ||
        (wrapperData->wState == WRAPPER_WSTATE_PAUSED)) {
        wrapperSetWrapperState(WRAPPER_WSTATE_STARTED);
        if (wrapperData->isDebugging) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG,
                TEXT("wrapperStopProcess(%d, %s) called while pausing or being paused."), exitCode, (force ? TEXT("TRUE") : TEXT("FALSE")));
        }
    }
    
    /* If we are are not aready shutting down, then do so. */
    if ((wrapperData->wState == WRAPPER_WSTATE_STOPPING) ||
        (wrapperData->wState == WRAPPER_WSTATE_STOPPED)) {
        if (wrapperData->isDebugging) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG,
                TEXT("wrapperStopProcess(%d, %s) called while stopping.  (IGNORED)"), exitCode, (force ? TEXT("TRUE") : TEXT("FALSE")));
        }
    } else {
        if (wrapperData->isDebugging) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG,
                TEXT("wrapperStopProcess(%d, %s) called."), exitCode, (force ? TEXT("TRUE") : TEXT("FALSE")));
        }
        /* If it has not already been set, set the exit request flag. */
        if (wrapperData->exitRequested ||
            (wrapperData->jState == WRAPPER_JSTATE_DOWN_CLEAN) ||
            (wrapperData->jState == WRAPPER_JSTATE_STOP) ||
            (wrapperData->jState == WRAPPER_JSTATE_STOPPING) ||
            (wrapperData->jState == WRAPPER_JSTATE_STOPPED) ||
            (wrapperData->jState == WRAPPER_JSTATE_KILLING) ||
            (wrapperData->jState == WRAPPER_JSTATE_KILLED) ||
            (wrapperData->jState == WRAPPER_JSTATE_KILL) ||
            (wrapperData->jState == WRAPPER_JSTATE_DOWN_CHECK) ||
            (wrapperData->jState == WRAPPER_JSTATE_DOWN_FLUSH_STDIN) ||
            (wrapperData->jState == WRAPPER_JSTATE_DOWN_FLUSH)) {
            /* JVM is already down or going down. */
        } else {
            wrapperData->exitRequested = TRUE;
        }

        wrapperData->exitCode = exitCode;

        if (force) {
            /* Make sure that further restarts are disabled. */
            wrapperData->restartRequested = WRAPPER_RESTART_REQUESTED_NO;

            /* Do not call wrapperSetWrapperState(WRAPPER_WSTATE_STOPPING) here.
             *  It will be called by the wrappereventloop.c.jStateDown once the
             *  the JVM is completely down.  Calling it here will make it
             *  impossible to trap and restart based on exit codes or other
             *  Wrapper configurations. */

            if (wrapperData->isDebugging) {
                if ((wrapperData->restartRequested == WRAPPER_RESTART_REQUESTED_AUTOMATIC) || (wrapperData->restartRequested == WRAPPER_RESTART_REQUESTED_CONFIGURED)) {
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("  Overriding request to restart JVM."));
                }
            }
        } else {
            /* Do not call wrapperSetWrapperState(WRAPPER_WSTATE_STOPPING) here.
             *  It will be called by the wrappereventloop.c.jStateDown once the
             *  the JVM is completely down.  Calling it here will make it
             *  impossible to trap and restart based on exit codes. */
            if (wrapperData->isDebugging) {
                if ((wrapperData->restartRequested == WRAPPER_RESTART_REQUESTED_AUTOMATIC) || (wrapperData->restartRequested == WRAPPER_RESTART_REQUESTED_CONFIGURED)) {
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("  Stop ignored.  Continuing to restart JVM."));
                }
            }
        }
    }
}

/**
 * Depending on the current state, we want to change the exact message displayed when restarting the JVM.
 *
 * The logic here needs to match that in wrapperRestartProcess.
 */
const TCHAR *wrapperGetRestartProcessMessage() {
    if ((wrapperData->jState == WRAPPER_JSTATE_DOWN_CLEAN) ||
        (wrapperData->jState == WRAPPER_JSTATE_STOP) ||
        (wrapperData->jState == WRAPPER_JSTATE_STOPPING) ||
        (wrapperData->jState == WRAPPER_JSTATE_STOPPED) ||
        (wrapperData->jState == WRAPPER_JSTATE_KILLING) ||
        (wrapperData->jState == WRAPPER_JSTATE_KILL) ||
        (wrapperData->jState == WRAPPER_JSTATE_DOWN_CHECK) ||
        (wrapperData->jState == WRAPPER_JSTATE_DOWN_FLUSH_STDIN) ||
        (wrapperData->jState == WRAPPER_JSTATE_DOWN_FLUSH) ||
        (wrapperData->jState == WRAPPER_JSTATE_LAUNCH_DELAY)) {
        if (wrapperData->restartRequested || (wrapperData->jState == WRAPPER_JSTATE_LAUNCH_DELAY)) {
            return TEXT("Restart JVM (Ignoring, already restarting).");
        } else {
            return TEXT("Restart JVM (Ignoring, already shutting down).");
        }
    } else if (wrapperData->exitRequested || wrapperData->restartRequested) {
        return TEXT("Restart JVM (Ignoring, already restarting).");
    } else {
        return TEXT("Restarting JVM.");
    }
}

/**
 * Depending on the current state, we want to change the exact message displayed when pausing the Wrapper.
 *
 * The logic here needs to match that in wrapperPauseProcess.
 */
const TCHAR *wrapperGetPauseProcessMessage() {
    if (!wrapperData->pausable) {
        return TEXT("Pause (Ignoring, the Wrapper was not set pausable).");
    } else if ((wrapperData->wState == WRAPPER_WSTATE_STOPPING) ||
               (wrapperData->wState == WRAPPER_WSTATE_STOPPED)) {
        return TEXT("Pause (Ignoring, already stopping)."); 
    } else if (wrapperData->wState == WRAPPER_WSTATE_PAUSING) {
        return TEXT("Pause (Ignoring, already pausing)."); 
    } else if (wrapperData->wState == WRAPPER_WSTATE_PAUSED) {
        return TEXT("Pause (Ignoring, already paused)."); 
    } else {
        return TEXT("Pausing..."); 
    }
}

/**
 * Depending on the current state, we want to change the exact message displayed when resuming the Wrapper.
 *
 * The logic here needs to match that in wrapperResumeProcess.
 */
const TCHAR *wrapperGetResumeProcessMessage() {
    if ((wrapperData->wState == WRAPPER_WSTATE_STOPPING) ||
        (wrapperData->wState == WRAPPER_WSTATE_STOPPED)) {
        return TEXT("Resume (Ignoring, already stopping)."); 
    } else if (wrapperData->wState == WRAPPER_WSTATE_STARTING) {
        return TEXT("Resume (Ignoring, already starting)."); 
    } else if (wrapperData->wState == WRAPPER_WSTATE_STARTED) {
        return TEXT("Resume (Ignoring, already started)."); 
    } else if (wrapperData->wState == WRAPPER_WSTATE_RESUMING) {
        return TEXT("Resume (Ignoring, already resuming)."); 
    } else {
        return TEXT("Resuming..."); 
    }
}

/**
 * Used to ask the state engine to shut down the JVM.  This are always intentional restart requests.
 */
void wrapperRestartProcess() {
    /* If it has not already been set, set the restart request flag in the wrapper data. */
    if (wrapperData->exitRequested || wrapperData->restartRequested ||
        (wrapperData->jState == WRAPPER_JSTATE_DOWN_CLEAN) ||
        (wrapperData->jState == WRAPPER_JSTATE_STOP) ||
        (wrapperData->jState == WRAPPER_JSTATE_STOPPING) ||
        (wrapperData->jState == WRAPPER_JSTATE_STOPPED) ||
        (wrapperData->jState == WRAPPER_JSTATE_KILLING) ||
        (wrapperData->jState == WRAPPER_JSTATE_KILL) ||
        (wrapperData->jState == WRAPPER_JSTATE_DOWN_CHECK) ||
        (wrapperData->jState == WRAPPER_JSTATE_DOWN_FLUSH_STDIN) ||
        (wrapperData->jState == WRAPPER_JSTATE_DOWN_FLUSH) ||
        (wrapperData->jState == WRAPPER_JSTATE_LAUNCH_DELAY)) { /* Down but not yet restarted. */

        if (wrapperData->isDebugging) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG,
                TEXT("wrapperRestartProcess() called.  (IGNORED)"));
        }
    } else {
        if (wrapperData->isDebugging) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG,
                TEXT("wrapperRestartProcess() called."));
        }

        wrapperData->exitRequested = TRUE;
        wrapperData->restartRequested = WRAPPER_RESTART_REQUESTED_CONFIGURED;
    }
}

/**
 * Used to ask the state engine to pause the JVM.
 *
 * @param actionSourceCode Tracks where the action originated.
 */
void wrapperPauseProcess(int actionSourceCode) {
    TCHAR msgBuffer[10];

    if (!wrapperData->pausable) {
        if (wrapperData->isDebugging) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT(
                "wrapperPauseProcess() called but wrapper.pausable is FALSE.  (IGNORED)"));
        }
        return;
    }

    if ((wrapperData->wState == WRAPPER_WSTATE_STOPPING) ||
        (wrapperData->wState == WRAPPER_WSTATE_STOPPED)) {
        /* If we are already shutting down, then ignore and continue to do so. */

        if (wrapperData->isDebugging) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT(
                "wrapperPauseProcess() called while stopping.  (IGNORED)"));
        }
    } else if (wrapperData->wState == WRAPPER_WSTATE_PAUSING) {
        /* If we are currently being paused, then ignore and continue to do so. */

        if (wrapperData->isDebugging) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT(
                "wrapperPauseProcess() called while pausing.  (IGNORED)"));
        }
    } else if (wrapperData->wState == WRAPPER_WSTATE_PAUSED) {
        /* If we are currently paused, then ignore and continue to do so. */

        if (wrapperData->isDebugging) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT(
                "wrapperPauseProcess() called while paused.  (IGNORED)"));
        }
    } else {
        if (wrapperData->isDebugging) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT(
                "wrapperPauseProcess() called."));
        }

        wrapperSetWrapperState(WRAPPER_WSTATE_PAUSING);

        if (!wrapperData->pausableStopJVM) {
            /* Notify the Java process. */
            _sntprintf(msgBuffer, 10, TEXT("%d"), actionSourceCode);
            wrapperProtocolFunction(WRAPPER_MSG_PAUSE, msgBuffer);
        }
    }
}

/**
 * Used to ask the state engine to resume a paused the JVM.
 *
 * @param actionSourceCode Tracks where the action originated.
 */
void wrapperResumeProcess(int actionSourceCode) {
    TCHAR msgBuffer[10];

    if ((wrapperData->wState == WRAPPER_WSTATE_STOPPING) ||
        (wrapperData->wState == WRAPPER_WSTATE_STOPPED)) {
        /* If we are already shutting down, then ignore and continue to do so. */

        if (wrapperData->isDebugging) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT(
                "wrapperResumeProcess() called while stopping.  (IGNORED)"));
        }
    } else if (wrapperData->wState == WRAPPER_WSTATE_STARTING) {
        /* If we are currently being started, then ignore and continue to do so. */

        if (wrapperData->isDebugging) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT(
                "wrapperResumeProcess() called while starting.  (IGNORED)"));
        }
    } else if (wrapperData->wState == WRAPPER_WSTATE_STARTED) {
        /* If we are currently started, then ignore and continue to do so. */

        if (wrapperData->isDebugging) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT(
                "wrapperResumeProcess() called while started.  (IGNORED)"));
        }
    } else if (wrapperData->wState == WRAPPER_WSTATE_RESUMING) {
        /* If we are currently being continued, then ignore and continue to do so. */

        if (wrapperData->isDebugging) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT(
                "wrapperResumeProcess() called while resuming.  (IGNORED)"));
        }
    } else {
        if (wrapperData->isDebugging) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT(
                "wrapperResumeProcess() called."));
        }

        /* If we were configured to stop the JVM then we want to reset its failed
         *  invocation count as the current stoppage was expected. */
        if (wrapperData->pausableStopJVM) {
            wrapperData->failedInvocationCount = 0;
        }

        wrapperSetWrapperState(WRAPPER_WSTATE_RESUMING);

        if (!wrapperData->pausableStopJVM) {
            /* Notify the Java process. */
            _sntprintf(msgBuffer, 10, TEXT("%d"), actionSourceCode);
            wrapperProtocolFunction(WRAPPER_MSG_RESUME, msgBuffer);
        }
    }
}

/**
 * Sends a command off to the JVM asking it to perform a garbage collection sweep.
 *
 * @param actionSourceCode Tracks where the action originated.
 */
void wrapperRequestJVMGC(int actionSourceCode) {
    TCHAR msgBuffer[10];
    
    /* Notify the Java process. */
    _sntprintf(msgBuffer, 10, TEXT("%d"), actionSourceCode);
    wrapperProtocolFunction(WRAPPER_MSG_GC, msgBuffer);
}

/**
 * Process a 'quotable' string by stripping the quotes and unescaping other
 *  characters.  If the bufferSize is not large enough then
 *  the required size will be returned.  0 is returned if successful. -1 or error.
 */
static int wrapperUnquoteArgInner(const TCHAR *val, size_t len, TCHAR* buffer, size_t bufferSize, const TCHAR *src, int isParameterFile, int allowSplit, int minLogLevel) {
    size_t i, j, k;
    int inQuote = FALSE;
    int firstSpacePos = -1;
    int quotePos = -1;
    int ok = FALSE;
    int error = FALSE;

    j = 0;
    for (i = 0; i < len; i++) {
        if ((val[i] == TEXT('\\')) && (i < len - 1)) {
            if (val[i + 1] == TEXT('\\')) {
                /* Double backslash.  Keep the first, and skip the second. */
                if (j < bufferSize) {
                    buffer[j] = val[i];
                }
                j++;
                i++;
            } else if (val[i + 1] == TEXT('"')) {
                /* Escaped quote.  Keep the quote. */
                if (j < bufferSize) {
                    buffer[j] = val[i + 1];
                }
                j++;
                i++;
            } else {
                /* This character should not be escaped. */
                if (minLogLevel <= LEVEL_FATAL) {
                    if (isParameterFile) {
                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Parameter file '%s' contains an invalid escaped character '%c'."), src, val[i + 1]);
                    } else {
                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("The value of property '%s' contains an invalid escaped character '%c'."), src, val[i + 1]);
                    }
                }
                error = TRUE;
            }
        } else if (val[i] == TEXT('"')) {
            /* Quote.  Skip it. */
            inQuote = !inQuote;

            if (quotePos == -1) {
                /* Opening quote. Check if the position is correct. */
                if (i == 0) {
                    /* First position -> OK! */
                    ok = TRUE;
                } else if (firstSpacePos != -1) {
                    /* There is a space before the opening quote. */
                    if (allowSplit) {
                        /* We need to allow these cases: [--opt "val"] & [--opt name="val"]. */
                        k = getJvmOptionSplitPos(val);
                        if (k == (size_t)firstSpacePos) {
                            while (val[k] == TEXT(' ')) {
                                k++;
                            }
                            if (k == i) {
                                /* [--opt "val"] -> OK! */
                                ok = TRUE;
                            } else if ((i > k + 1) && (val[i - 1] == TEXT('='))) {
                                /* Make sure there is no other space before the quote. */
                                while (val[k] != TEXT(' ')) {
                                    k++;
                                    if (k == i - 1) {
                                        /* [--opt name="val"] -> OK! */
                                        ok = TRUE;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                } else if (val[i - 1] == TEXT('=')) {
                    /* [-Dprop="val"] -> OK! */
                    ok = TRUE;
                }

                if (!ok) {
                    if (minLogLevel <= LEVEL_FATAL) {
                        if (isParameterFile) {
                            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Parameter file '%s' contains an argument with an opening quote in the wrong position."), src);
                        } else {
                            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("The value of property '%s' contains an opening quote in the wrong position."), src);
                        }
                    }
                    error = TRUE;
                }
            }
            quotePos = (int)i;
        } else {
            if ((val[i] == TEXT(' ')) && (firstSpacePos == -1)) {
                firstSpacePos = (int)i;
            }
            if (j < bufferSize) {
                buffer[j] = val[i];
            }
            j++;
        }
    }

    /* Null terminate. */
    if (j < bufferSize) {
        buffer[j] = TEXT('\0');
    }
    j++;

    if (inQuote) {
        if (minLogLevel <= LEVEL_FATAL) {
            if (isParameterFile) {
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Parameter file '%s' contains an unclosed quote."), src);
            } else {
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("The value of property '%s' contains an unclosed quote."), src);
            }
        }
        error = TRUE;
    } else if ((quotePos > 0) && (quotePos < (int)(len - 1))) {
        if (minLogLevel <= LEVEL_FATAL) {
            if (isParameterFile) {
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Parameter file '%s' contains an argument with a closing quote in the wrong position."), src);
            } else {
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("The value of property '%s' contains a closing quote in the wrong position."), src);
            }
        }
        error = TRUE;
    }

    if (error) {
        return -1;
    } else if (j <= bufferSize) {
        return 0;
    } else {
        return (int)j;
    }
}

TCHAR* wrapperUnquoteArg(const TCHAR *val, const TCHAR *src, int isParameterFile, int allowSplit, int minLogLevel) {
    size_t len;
    TCHAR* buffer = NULL;
    int req;

    len = _tcslen(val);
    req = wrapperUnquoteArgInner(val, len, NULL, 0, src, isParameterFile, allowSplit, minLogLevel);
    if (req > 0) {
        buffer = malloc(sizeof(TCHAR) * req);
        if (!buffer) {
            outOfMemory(TEXT("UQ"), 1);
        } else {
            wrapperUnquoteArgInner(val, len, buffer, req, src, isParameterFile, allowSplit, minLogLevel);
        }
    }
    return buffer;
}

/**
 * Trim outer quotes if any.
 *  This function does not allocate a new string.
 */
static TCHAR* removeOuterQuotes(TCHAR *val) {
    size_t len = _tcslen(val);

    if ((val[0] == TEXT('"')) && (val[len - 1] == TEXT('"'))) {
        val[len - 1] = 0;
        return val + 1;
    }
    return val;
}

#ifdef WIN32
int addOuterQuotes(TCHAR **pVal) {
    TCHAR* result;
    size_t len = _tcslen(*pVal);

    if ((*pVal)[0] == TEXT('"')) {
        /* Already quoted. */
    } else {
        result = malloc(sizeof(TCHAR) * (len + 2 + 1));
        if (!result) {
            outOfMemory(TEXT("AOQ"), 1);
            return TRUE;
        }
        _sntprintf(result, len + 2 + 1, TEXT("\"%s\""), *pVal);
        if (*pVal) {
            free(*pVal);
        }
        *pVal = result;
    }
    return FALSE;
}

#else
int checkIfExecutable(const TCHAR *filename) {
    int result;
    struct stat statInfo;

    result = _tstat(filename, &statInfo);
    if (result < 0) {
        return 0;
    }

    if (!S_ISREG(statInfo.st_mode)) {
        return 0;
    }
    if (statInfo.st_uid == geteuid()) {
        return statInfo.st_mode & S_IXUSR;
    }
    if (statInfo.st_gid == getegid()) {
        return statInfo.st_mode & S_IXGRP;
    }
    return statInfo.st_mode & S_IXOTH;
}
#endif

int checkIfBinary(const TCHAR *filename) {
    FILE* f;
    unsigned char head[5];
    int r;
    f = _tfopen(filename, TEXT("rb"));
    if (!f) { /*couldnt find the java command... wrapper will moan later*/
       return 1;
    } else {
        r = (int)fread( head,1, 4, f);
        if (r != 4)
        {
            fclose(f);
            return 0;
        }
        fclose(f);
        head[4] = '\0';
        if (wrapperData->isDebugging) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Magic number for file %s: 0x%02x%02x%02x%02x"), filename, head[0], head[1], head[2], head[3]);
        }

#if defined(LINUX) || defined(FREEBSD) || defined(SOLARIS) 
        if (head[1] == 'E' && head[2] == 'L' && head[3] == 'F') {
            return 1; /*ELF */
#elif defined(AIX)
        /* http://en.wikipedia.org/wiki/XCOFF */
        if (head[0] == 0x01 && head[1] == 0xf7 && head[2] == 0x00) { /* 0x01f700NN */
            return 1; /*xcoff 64*/
        } else if (head[0] == 0x01 && head[1] == 0xdf && head[2] == 0x00) { /* 0x01df00NN */
            return 1; /*xcoff 32*/
#elif defined(MACOSX)
        if (head[0] == 0xca && head[1] == 0xfe && head[2] == 0xba && head[3] == 0xbe) { /* 0xcafebabe */
            return 1; /*MACOS Universal binary*/
        } else if (head[0] == 0xcf && head[1] == 0xfa && head[2] == 0xed && head[3] == 0xfe) { /* 0xcffaedfe */
            return 1; /*MACOS x86_64 binary*/
        } else if (head[0] == 0xce && head[1] == 0xfa && head[2] == 0xed && head[3] == 0xfe) { /* 0xcefaedfe */
            return 1; /*MACOS i386 binary*/
        } else if (head[0] == 0xfe && head[1] == 0xed && head[2] == 0xfa && head[3] == 0xce) { /* 0xfeedface */
            return 1; /*MACOS ppc, ppc64 binary*/
#elif defined(HPUX)
        if (head[0] == 0x02 && head[1] == 0x10 && head[2] == 0x01 && head[3] == 0x08) { /* 0x02100108 PA-RISC 1.1 */
            return 1; /*HP UX PA RISC 32*/
        } else if (head[0] == 0x02 && head[1] == 0x14 && head[2] == 0x01 && head[3] == 0x07) { /* 0x02140107 PA-RISC 2.0 */
            return 1; /*HP UX PA RISC 32*/
        } else if (head[1] == 'E' && head[2] == 'L' && head[3] == 'F') {
            return 1; /*ELF */
#elif defined(WIN32)
        if (head[0] == 'M' && head[1] == 'Z') {
            return 1; /* MS */
#else
        if (FALSE) {
 #error I dont know what to do for this host type. (in checkIfBinary())
#endif
        } else {
            return 0;
        }
    }
}

/**
 * Indicates if the Java command is jdb
 *
 * @param path to the Java command. Assumed not NULL.
 * @param whether the path is surrounded with quotes or not.
 *
 * @return TRUE if this is jdb, FALSE otherwise.
 */
int isCommandJdb(const TCHAR* path) {
    const TCHAR* fileName;
    
    fileName = getFileName(path);
    if ((_tcscmp(fileName, TEXT("jdb")) == 0)
#ifdef WIN32
     || (_tcscmp(fileName, TEXT("jdb.exe")) == 0)
#endif
        ) {
        return TRUE;
    }
    return FALSE;
}

#ifdef WIN32
/**
 * Indicates if the Java command is javaw
 *
 * @param path to the Java command. Assumed not NULL.
 * @param whether the path is surrounded with quotes or not.
 *
 * @return TRUE if this is javaw, FALSE otherwise.
 */
int isCommandJavaw(const TCHAR* path) {
    const TCHAR* fileName;
    
    fileName = getFileName(path);
    if ((_tcscmp(fileName, TEXT("javaw.exe")) == 0) ||
        (_tcscmp(fileName, TEXT("javaw")) == 0)) {
        return TRUE;
    }
    return FALSE;
}

#else
/**
 * Searches for an executable file and returns its absolute path if it is found.
 *  NOTE: This function behaves like Windows it receives a file name without relative path components
 *  (it first searches in the current directory, then in PATH), and like UNIX otherwise (UNIX terminals
 *  require to append './' before the executable name when launching it, so there is no fallback to the PATH).
 *  We may change this in the future.
 *
 * @param exe  Path to the binary to search.
 * @param name Label to use when printing debug output.
 * @param compat TRUE for compatibility mode which searches the file name in the current directory,
 *               FALSE to require './' for the current directory.
 *
 * @return The absolute path to the file if it is found and executable,
 *         otherwise NULL and errno is set to indicate the error.
 */
TCHAR* findPathOf(const TCHAR *exe, const TCHAR *name, int compat) {
    TCHAR *searchPath;
    TCHAR *beg, *end;
    int stop, found;
    TCHAR pth[PATH_MAX + 1];
    TCHAR *ret;
    TCHAR resolvedPath[PATH_MAX + 1];
    int err = ENOENT;

    if (exe[0] == TEXT('/')) {
        /* This is an absolute reference. */
        if (_trealpathN(exe, resolvedPath, PATH_MAX + 1)) {
            _tcsncpy(pth, resolvedPath, PATH_MAX + 1);
            if (checkIfExecutable(pth)) {
                ret = malloc((_tcslen(pth) + 1) * sizeof(TCHAR));
                if (!ret) {
                    outOfMemory(TEXT("FPO"), 1);
                    errno = ENOMEM;
                    return NULL;
                }
                _tcsncpy(ret, pth, _tcslen(pth) + 1);
                if (wrapperData->isDebugging) {
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Resolved the real path of %s as an absolute reference: %s"), name, ret);
                }
                errno = 0;
                return ret;
            } else {
                err = EACCES;
            }
        } else {
            err = errno;
        }
        if (wrapperData->isDebugging) {
            if (_tcslen(resolvedPath)) {
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Unable to resolve the real path of %s as an absolute reference: %s. %s (Problem at: %s)"), name, exe, getLastErrorText(), resolvedPath);
            } else {
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Unable to resolve the real path of %s as an absolute reference: %s. %s"), name, exe, getLastErrorText());
            }
        }
        errno = err;
        return NULL;
    }

    if (compat || (_tcschr(exe, TEXT('/')) != NULL)) {
        /* This is a non-absolute reference.  See if it is a relative reference. */
        if (_trealpathN(exe, resolvedPath, PATH_MAX + 1)) {
            /* Resolved.  See if the file exists. */
            _tcsncpy(pth, resolvedPath, PATH_MAX + 1);
            if (checkIfExecutable(pth)) {
                ret = malloc((_tcslen(pth) + 1) * sizeof(TCHAR));
                if (!ret) {
                    outOfMemory(TEXT("FPO"), 2);
                    errno = ENOMEM;
                    return NULL;
                }
                _tcsncpy(ret, pth, _tcslen(pth) + 1);
                if (wrapperData->isDebugging) {
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Resolved the real path of %s as a relative reference: %s"), name, ret);
                }
                errno = 0;
                return ret;
            } else {
                /* Set err, but we may clear it if searching in the PATH. */
                err = EACCES;
            }
        } else {
            err = errno;
        }
        if (wrapperData->isDebugging) {
            if (_tcslen(resolvedPath)) {
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Unable to resolve the real path of %s as a relative reference: %s. %s (Problem at: %s)"), name, exe, getLastErrorText(), resolvedPath);
            } else {
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Unable to resolve the real path of %s as a relative reference: %s. %s"), name, exe, getLastErrorText());
            }
        }
    }

    /* The file was not a direct relative reference.   If and only if it does not contain any relative path components, we can search the PATH. */
    if (_tcschr(exe, TEXT('/')) == NULL) {
        /* On UNIX, if a file is referenced twice in the PATH, the first location where the file has the correct permission will be chosen.
         *  Any file with insufficient permissions will be considered as not found. Set err to "No such file or directory". */
        err = ENOENT;
        searchPath = _tgetenv(TEXT("PATH"));
        if (searchPath && (_tcslen(searchPath) <= 0)) {
#if defined(UNICODE)
            free(searchPath);
#endif
            searchPath = NULL;
        }
        if (searchPath) {
            if (wrapperData->isDebugging) {
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Attempt to locate %s on system PATH: %s"), name, exe);
            }

            beg = searchPath;
            stop = 0; found = 0;
            do {
                end = _tcschr(beg, TEXT(':'));
                if (end == NULL) {
                    /* This is the last element in the PATH, so we want the whole thing. */
                    stop = 1;
                    _tcsncpy(pth, beg, PATH_MAX + 1);
                } else {
                    /* Copy the single path entry. */
                    _tcsncpy(pth, beg, end - beg);
                    pth[end - beg] = TEXT('\0');
                }
                if (pth[_tcslen(pth) - 1] != TEXT('/')) {
                    _tcsncat(pth, TEXT("/"), PATH_MAX + 1);
                }
                _tcsncat(pth, exe, PATH_MAX + 1);

                /* The file can exist on the path, but via a symbolic link, so we need to expand it.  Ignore errors here. */
#ifdef _DEBUG
                if (wrapperData->isDebugging) {
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("  Check PATH entry: %s"), pth);
                }
#endif
                if (_trealpathN(pth, resolvedPath, PATH_MAX + 1) != NULL) {
                    /* Copy over the result. */
                    _tcsncpy(pth, resolvedPath, PATH_MAX + 1);
                    found = checkIfExecutable(pth);
                } else if (wrapperData->isDebugging) {
                    if (_tcslen(resolvedPath)) {
                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Unable to resolve the path %s: %s (Problem at: %s)"), pth, getLastErrorText(), resolvedPath);
                    } else {
                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Unable to resolve the path %s: %s"), pth, getLastErrorText());
                    }
                    stop = TRUE;
                }

                if (!stop) {
                    beg = end + 1;
                }
            } while (!stop && !found);

#if defined(UNICODE)
            free(searchPath);
#endif

            if (found) {
                ret = malloc((_tcslen(pth) + 1) * sizeof(TCHAR));
                if (!ret) {
                    outOfMemory(TEXT("FPO"), 3);
                    errno = ENOMEM;
                    return NULL;
                }
                _tcsncpy(ret, pth, _tcslen(pth) + 1);
                if (wrapperData->isDebugging) {
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Resolved the real path of %s from system PATH: %s"), name, ret);
                }
                errno = 0;
                return ret;
            } else if (wrapperData->isDebugging) {
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Unable to resolve the real path of %s on the system PATH: %s"), name, exe);
            }
        }
    }

    /* Still did not find the file.  So it must not exist. */
    errno = err;
    return NULL;
}
#endif

/**
 * Checks to see if the specified executable is a regular binary.
 *  NOTE: On UNIX, this function will also resolve the java command if wrapper.java.command.resolve is set to TRUE.
 *
 * @param para The binary to check.  On UNIX, the para memory may be freed and reallocated by this call.
 */
void checkIfRegularExe(TCHAR** para) {
    TCHAR* path;
#ifdef WIN32
    int len, start;

    if (_tcschr(*para, TEXT('\"')) != NULL){
        start = 1;
        len = (int)_tcslen(*para) - 2;
    } else {
        start = 0;
        len = (int)_tcslen(*para);
    }
    path = malloc(sizeof(TCHAR) * (len + 1));
    if (!path){
        outOfMemory(TEXT("CIRE"), 1);
    } else {
        _tcsncpy(path, (*para) + start, len);
        path[len] = TEXT('\0');
#else
    int replacePath;

    path = findPathOf(*para, TEXT("wrapper.java.command"), TRUE);
    if (!path) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN, TEXT("The configured wrapper.java.command could not be found, attempting to launch anyway: %s"), *para);
    } else {
        replacePath = getBooleanProperty(properties, TEXT("wrapper.java.command.resolve"), TRUE);
        if (replacePath) {
            free(*para);
            *para = malloc((_tcslen(path) + 1) * sizeof(TCHAR));
            if (!(*para)) {
                outOfMemory(TEXT("CIRE"), 2);
                free(path);
                return;
            }
            _tcsncpy(*para, path, _tcslen(path) + 1);
        }
#endif
        if (!checkIfBinary(path)) {
            /* Set a flag to later show a warning when receiving the Java PID from the backend. */
            wrapperData->javaCommandNotBinary = TRUE;
        }
        free(path);
    }
}

#ifdef WIN32
void wrapperCheckMonitoredProcess(DWORD javaPID) {
    HANDLE hProc;
    TCHAR realJavaPath[_MAX_PATH];
    int adviseRealPath = FALSE;

    /* Check if the PID of the Java process at the moment of the fork is the same as the one returned through backend. */
    if (wrapperData->javaPID != javaPID) {
        if (wrapperData->javaCommandNotBinary == FALSE) {
            /* Java was launched via a redirector binary (such as C:\Program Files\Common Files\Oracle\Java\javapath\java.exe)
             *  which PID differs from the real Java process. Unlike scripts (which are often user-made, editable or able to
             *  log their own warnings), redirector binaries make it pretty difficult for the user to figure out the target path.
             *  To help the user, get a handle to the Java process and print its executable path.
             *  (Note: for Oracle binaries, a way to confirm the real path is to use the 'java -verbose' command) */
            if (hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, javaPID)) {
                if (GetModuleFileNameEx(hProc, NULL, realJavaPath, _MAX_PATH)) {
                    adviseRealPath = TRUE;
                }
                CloseHandle(hProc);
            }
        }
        
        if (adviseRealPath) {
            log_printf(WRAPPER_SOURCE_WRAPPER, wrapperData->monitorRedirectLogLevel, TEXT("Detected that the Java process launched by the Wrapper (PID %d) was redirected to %s (PID %d)."), wrapperData->javaPID, realJavaPath, javaPID);
        } else {
            log_printf(WRAPPER_SOURCE_WRAPPER, wrapperData->monitorRedirectLogLevel, TEXT("The PID (%d) of the running Java process differs from the PID (%d) of the process launched by the Wrapper."), javaPID, wrapperData->javaPID);
        }
        if (wrapperData->javaCommandNotBinary) {
            log_printf(WRAPPER_SOURCE_WRAPPER, wrapperData->monitorRedirectLogLevel, TEXT(" The value of wrapper.java.command does not appear to be a Java binary."));
        }
        
        if (!wrapperData->monitorLaunched) {
            /* We monitor the Java process, so update its PID and handle. */
            if (hProc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, javaPID)) {
                /* We got the handle. Print a message before the PID gets updated. */
                log_printf(WRAPPER_SOURCE_WRAPPER, wrapperData->monitorRedirectLogLevel, TEXT(" Switch to monitoring the Java process."));
                CloseHandle(wrapperData->javaProcess);
                wrapperData->javaProcess = hProc;
                wrapperData->javaPID = javaPID;
                
                /* Update the Java pid file. */
                if (wrapperData->javaPidFilename) {
                    if (writePidFile(wrapperData->javaPidFilename, wrapperData->javaPID, wrapperData->javaPidFileUmask)) {
                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN,
                            TEXT("Unable to write the Java PID file: %s"), wrapperData->javaPidFilename);
                    }
                }
            } else {
                /* Should not happen... */
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT(" Failed to switch to monitoring the Java process. %s"), getLastErrorText());
                wrapperStopProcess(wrapperData->errorExitCode, TRUE);
            }
        } else {
            log_printf(WRAPPER_SOURCE_WRAPPER, wrapperData->monitorRedirectLogLevel, TEXT(" Trying to continue, but some features such as thread dump requests or signal handling may not work correctly."));
        }
    }
}
#else

void wrapperCheckMonitoredProcess(pid_t javaPID) {
    /* Check if the PID of the Java process at the moment of the fork is the same as the one returned through backend. */
    if (wrapperData->javaPID != javaPID) {
        log_printf(WRAPPER_SOURCE_WRAPPER, wrapperData->monitorRedirectLogLevel, TEXT("The PID (%d) of the running Java process differs from the PID (%d) of the process launched by the Wrapper."), javaPID, wrapperData->javaPID);
        if (wrapperData->javaCommandNotBinary) {
            /* Should be rare (non-standard). In a Shell Script, this can happen if java is called without 'exec'. */
            log_printf(WRAPPER_SOURCE_WRAPPER, wrapperData->monitorRedirectLogLevel, TEXT(" The value of wrapper.java.command does not appear to be a Java binary."));
        }
        
        log_printf(WRAPPER_SOURCE_WRAPPER, wrapperData->monitorRedirectLogLevel, TEXT(" Trying to continue, but some features such as thread dump requests or signal handling may not work correctly."));
    }
}
#endif

/**
 * Builds up the java command section of the Java command line.
 *
 * @param shallow This parameter is used when only the command name is needed, regardless its location, nature, etc.
 *
 * @return The final index into the strings array, or -1 if there were any problems.
 */
int wrapperBuildJavaCommandArrayJavaCommand(TCHAR **strings, int index, int shallow) {
    const TCHAR *prop;
#ifdef WIN32
    TCHAR cpPath[512];
    int found;
#endif

    if (strings) {
        prop = getStringProperty(properties, TEXT("wrapper.java.command"), TEXT("java"));

#ifdef WIN32
        found = 0;

        if (_tcscmp(prop, TEXT("")) == 0) {
            /* If the java command is an empty string, we want to look for the
             *  the java command in the windows registry. */
            if (wrapperGetJavaHomeFromWindowsRegistry(cpPath)) {
                if (wrapperData->isDebugging && !shallow) {
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG,
                        TEXT("Loaded java home from registry: %s"), cpPath);
                }

                updateStringValue(&wrapperData->registry_java_home, cpPath);

                _tcsncat(cpPath, TEXT("\\bin\\java.exe"), 512);
                if (wrapperData->isDebugging && !shallow) {
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG,
                        TEXT("Found Java Runtime Environment home directory in system registry."));
                }
                found = 1;
            } else {
                if (!shallow) {
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL,
                        TEXT("The Java Runtime Environment home directory could not be located in the system registry."));
                }
                found = 0;
                return -1;
            }
        } else {
            updateStringValue(&wrapperData->registry_java_home, NULL);

            /* To avoid problems on Windows XP systems, the '/' characters must
             *  be replaced by '\' characters in the specified path.
             * prop is supposed to be constant, but allow this change as it is
             *  the actual value that we want. */
            wrapperCorrectWindowsPath((TCHAR *)prop);

            /* If the full path to the java command was not specified, then we
             *  need to try and resolve it here to avoid problems later when
             *  calling CreateProcess.  CreateProcess will look in the windows
             *  system directory before searching the PATH.  This can lead to
             *  the wrong JVM being run. */
            _sntprintf(cpPath, 512, TEXT("%s"), prop);
            if ((PathFindOnPath((TCHAR*)cpPath, (TCHAR**)wrapperGetSystemPath())) && (!PathIsDirectory(cpPath))) {
                /*printf("Found %s on path.\n", cpPath); */
                found = 1;
            } else {
                /*printf("Could not find %s on path.\n", cpPath); */

                /* Try adding .exe to the end */
                _sntprintf(cpPath, 512, TEXT("%s.exe"), prop);
                if ((PathFindOnPath(cpPath, wrapperGetSystemPath())) && (!PathIsDirectory(cpPath))) {
                    /*printf("Found %s on path.\n", cpPath); */
                    found = 1;
                } else {
                    /*printf("Could not find %s on path.\n", cpPath); */
                }
            }
        }
        setInternalVarProperty(properties, TEXT("WRAPPER_JAVA_HOME"), wrapperData->registry_java_home, FALSE, FALSE);

        if (found) {
            strings[index] = malloc(sizeof(TCHAR) * (_tcslen(cpPath) + 2 + 1));
            if (!strings[index]) {
                outOfMemory(TEXT("WBJCAJC"), 1);
                return -1;
            }
            _sntprintf(strings[index], _tcslen(cpPath) + 2 + 1, TEXT("%s"), cpPath);
        } else {
            strings[index] = malloc(sizeof(TCHAR) * (_tcslen(prop) + 2 + 1));
            if (!strings[index]) {
                outOfMemory(TEXT("WBJCAJC"), 2);
                return -1;
            }
            _sntprintf(strings[index], _tcslen(prop) + 2 + 1, TEXT("%s"), prop);
        }

#else /* UNIX */

        strings[index] = malloc(sizeof(TCHAR) * (_tcslen(prop) + 2 + 1));
        if (!strings[index]) {
            outOfMemory(TEXT("WBJCAJC"), 3);
            return -1;
        }
        _sntprintf(strings[index], _tcslen(prop) + 2 + 1, TEXT("%s"), prop);
#endif
        if (!shallow) {
            checkIfRegularExe(&strings[index]);
        }
    }

    index++;

    return index;
}

/**
 * The Java version will not parse correctly if the Java command is set to 'jdb' or 'javaw'.
 *  - jdb has a different output when the '-version' parameters is specified.
 *  - javaw has no std output.
 *  This function tries to resolve the command used to request the Java version by searching
 *  for the 'java' command located in the same directory, if it exists. If it doesn't, or if
 *  the directory can't be resolved, a warning will be display and execution will continue to
 *  let the Java version be resolved to its default value. 
 *
 * @param pCommand pointer to the Java command (the value may be modified).
 *
 * @return TRUE if we ran out of memory, FALSE otherwise. 
 */
int wrapperResolveJavaVersionCommand(TCHAR **pCommand) {
#ifdef WIN32
    const TCHAR* const ext = TEXT(".exe");
#else
    const TCHAR* const ext = TEXT("");
#endif
#if defined(WIN32) && !defined(WIN64)
    struct _stat64i32 fileStat;
#else
    struct stat fileStat;
#endif
    TCHAR* resolvedCommand;
    TCHAR* command;
    TCHAR* path1 = NULL;
    TCHAR* path2 = NULL;
    TCHAR* ptr = NULL;
    TCHAR c;
    int resolved = FALSE;
    size_t len;
    int error = 0;
    int result = FALSE;
    
    if (isCommandJdb(*pCommand)
#ifdef WIN32
     || isCommandJavaw(*pCommand)
#endif
        ) {
        command = *pCommand;
        
        /* This function returns a pointer to the file name. */
        ptr = (TCHAR*)getFileName(command);
        if (!ptr) {
            /* Should never happen if the implementation of the function doesn't change. */
            error = 10;
        } else if (command == ptr) {
            /* The command is specified without path. */
#ifdef WIN32
            /* On Windows this should never happen because wrapperBuildJavaCommandArrayJavaCommand always
             *  resolves the path to the command using the system PATH. Still log a warning just in case. */
            error = 1;
#else
            /* On Unix the Java command may not be resolved, but we need to make sure that we are targeting
             *  the same installation directory when calling 'java' without path. */
            if (getBooleanProperty(properties, TEXT("wrapper.java.command.resolve"), TRUE)) {
                /* The only possible reason is that findPathOf already failed in checkIfRegularExe.
                 *  An error has already been logged but we also want to show a warning for the Java version. */
                error = 1;
            } else {
                /* Get the full path to the Jdb/Javaw command. 3d param is FALSE to only resolve using system PATH (like the OS). */
                path1 = findPathOf(command, command, FALSE);
                if (!path1) {
                    error = 2;
                } else {
                    ptr = (TCHAR*)getFileName(path1);
                    if (!ptr) {
                        /* Should never happen */
                        error = 11;
                    } else {
                        /* Truncate path1 to remove the file name. */
                        *ptr = 0;
                        
                        /* Get the full path to the Java command. 3d param is FALSE to only resolve using system PATH (like the OS). */
                        path2 = findPathOf(TEXT("java"), TEXT("java"), FALSE);
                        if (!path2) {
                            error = 3;
                        } else {
                            ptr = (TCHAR*)getFileName(path2);
                            if (!ptr) {
                                /* Should never happen */
                                error = 12;
                            } else {
                                /* Truncate path2 to remove the file name. */
                                *ptr = 0;

                                if (_tcscmp(path1, path2) != 0) {
                                    error = 4;
                                } else {
                                    updateStringValue(pCommand, TEXT("java"));
                                    if (*pCommand) {
                                        resolved = TRUE;
                                    }
                                }
                            }
                            free(path2);
                        }
                    }
                    free(path1);
                }
            }
#endif
        } else {
            /* The command is specified with a path. */
            
            /* Truncate the command after the last separator and replace the command with 'java(.exe)'. */
            c = *ptr;
            *ptr = 0;
            len = _tcslen(command) + 4 + _tcslen(ext) + 1;
            resolvedCommand = malloc(sizeof(TCHAR) * len);
            if (!resolvedCommand) {
                outOfMemory(TEXT("WRJVC"), 2);
                return TRUE;
            } else {
                _sntprintf(resolvedCommand, len, TEXT("%sjava%s"), command, ext);
                if (_tstat(resolvedCommand, &fileStat) != 0) {
                    /* The 'java' command did not exist in the same directory */
                    error = 5;
                } else {
                    updateStringValue(pCommand, resolvedCommand);
                    if (*pCommand) {
                        resolved = TRUE;
                    }
                }
                free(resolvedCommand);
            }
            /* restore command because we want to use it for logging. */
            *ptr = c;
        }
        if (resolved) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Command used to query the Java version resolved to %s."), *pCommand);
        } else if (*pCommand == NULL) {
            /* OOM already reported */
            result = TRUE;
        } else {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN, TEXT("Could not find the 'java%s' command in the same directory as '%s' (Internal error %d).\n The Java version may not be resolved correctly."), ext, command, error);
            if (wrapperData->isAdviserEnabled) {
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ADVICE, TEXT(" You may use the wrapper.java.version.fallback property to set the version manually."));
            }
        }
    }
    if (!result) {
#ifdef WIN32
        /* Quote the command (we want this to be done after checking the command, as it is easier to always check it without quotes, but before setting wrapperData->javaQryCmd). */
        if (addOuterQuotes(pCommand)) {
            return TRUE;
        }
#endif
        /* Keep the value for other queries (Note: This works but is not perfect because it may not be where we expect the variable to be defined and stores the same command twice in memory) */
        updateStringValue(&wrapperData->javaQryCmd, *pCommand);
    }
    return result;
}

/**
 * Builds up the additional section of the Java command line.
 *
 * @return The final index into the strings array, or -1 if there were any problems.
 */
int wrapperBuildJavaCommandArrayJavaAdditional(TCHAR **strings, int detectDebugJVM, int target, int allowSplit, int index) {
    TCHAR *prop;
    TCHAR *prop2 = NULL;
    int i;
    size_t len;
    TCHAR paramBuffer1[128];
    int isQuotable;
    TCHAR **propertyNames;
    TCHAR **propertyValues;
    long unsigned int *propertyIndices;
    int logWarning = FALSE;
    int collectAppOnlyIndexes = FALSE;
    const TCHAR* scopeStr;
    int scope;
    size_t splitShift = 0;

    if (getStringProperties(properties, TEXT("wrapper.java.additional."), TEXT(""), wrapperData->ignoreSequenceGaps, FALSE, &propertyNames, &propertyValues, &propertyIndices)) {
        /* Failed */
        return -1;
    }

    if (target == JVM_TARGET_DRYRUN_APP) {
        if (!strings) {
            /* Only log warnings on the first pass (if there is only one property, the message would not be logged on the second pass). */
            logWarning = TRUE;

            /* If your Java version is 9 or higher, remember the arguments to ignore when calling --dry-run.
             *  Also do this on the first pass to catch errors earlier. */
            if (isJavaGreaterOrEqual(wrapperData->javaVersion, TEXT("9"))) {
                free(wrapperData->appOnlyAdditionalIndexes);
                wrapperData->appOnlyAdditionalIndexes = NULL;
                wrapperData->appOnlyAdditionalCount = 0;

                collectAppOnlyIndexes = TRUE;
            }
        }
    }

    i = 0;
    while (propertyNames[i]) {
        prop = propertyValues[i];
        if (prop && (_tcslen(prop) > 0)) {
            _sntprintf(paramBuffer1, 128, TEXT("wrapper.java.additional.%lu.scope"), propertyIndices[i]);
            scopeStr = getStringProperty(properties, paramBuffer1, NULL);
            if (scopeStr) {
                scope = getAdditionalScope(scopeStr);
                if (scope == JVM_TARGET_NONE) {
                    scope = getRealAdditionalScope(prop, JVM_TARGET_DRYRUN_APP, NULL, FALSE);
                    if (logWarning && (strcmpIgnoreCase(scopeStr, getAdditionalScopeStr(scope)) != 0)) {
                        log_printf(WRAPPER_SOURCE_WRAPPER, properties->logWarningLogLevel, 
                            TEXT("Encountered an invalid value for configuration property %s=%s.  Resolving to %s."),
                            paramBuffer1, scopeStr, getAdditionalScopeStr(scope));
                    }
                } else {
                    scope = getRealAdditionalScope(prop, scope, paramBuffer1, logWarning);
                }
            } else {
                _sntprintf(paramBuffer1, 128, TEXT("wrapper.java.additional.%lu.app_only"), propertyIndices[i]);
                if (!getStringProperty(properties, paramBuffer1, NULL)) {
                    scope = getRealAdditionalScope(prop, JVM_TARGET_DRYRUN_APP, NULL, FALSE);
                } else if (getBooleanProperty(properties, paramBuffer1, FALSE)) {
                    scope = getRealAdditionalScope(prop, JVM_TARGET_APP, paramBuffer1, logWarning);
                } else {
                    scope = getRealAdditionalScope(prop, JVM_TARGET_DRYRUN_APP, paramBuffer1, logWarning);
                }
            }

            if (scope == JVM_TARGET_NONE) {
                /* This option should not appear on any command line. */
                i++;
                continue;
            } else if (target == JVM_TARGET_BOOTSTRAP) {
                /* Check if this parameter should be excluded from the bootstrap command line. */
                if (!(scope & JVM_TARGET_BOOTSTRAP)) {
                    i++;
                    continue;
                }
            } else if (collectAppOnlyIndexes) {
                /* Check if this parameter should be excluded from the "--dry-run" command line". */
                if (!(scope & JVM_TARGET_DRYRUN)) {
                    if (addAppOnlyAdditional(index)) {
                        index = -1;
                        break;
                    }
                }
            } else {
                /* Unless the scope JVM_TARGET_NONE, an option should always appear on the app command line. */
            }

            _sntprintf(paramBuffer1, 128, TEXT("wrapper.java.additional.%lu.quotable"), propertyIndices[i]);
            isQuotable = getBooleanProperty(properties, paramBuffer1, FALSE);
            if (isQuotable) {
                if (target == JVM_TARGET_ALL) {
                    /* In this call we build the internal array. Never report problems; this will be done in subsequent calls. */

                    /* This call itself will not split any options, but we need to allow unquoting splittable options. */
                    prop2 = wrapperUnquoteArg(prop, propertyNames[i], FALSE, TRUE, LEVEL_NONE);
                    if (!prop2) {
                        /* The error is not related to the position of the quotes or to unclosed quotes.
                         *  We will report it later. Skip. */
                        i++;
                        continue;
                    }
                } else {
                    prop2 = wrapperUnquoteArg(prop, propertyNames[i], FALSE, allowSplit, LEVEL_ERROR);
                    if (!prop2) {
                        /* error reported */
                        index = -1;
                        break;
                    }
                }
                prop = prop2;
            }

            /* All additional parameters must begin with a - or they will be interpreted as being the main class name by Java. */
            if (!(_tcsstr(prop, TEXT("-")) == prop)) {
                /* Only log the message on the second pass. */
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL,
                    TEXT("The value of property '%s' (%s) is not a valid argument to the JVM."),
                    propertyNames[i], prop);
                index = -1;
                break;
            } else {
                if (allowSplit) {
                    len = getJvmOptionSplitPos(prop);
                    if (len > 0) {
                        if (strings) {
                            /* The option doesn't need to be escaped. */
                            strings[index] = malloc(sizeof(TCHAR) * (len + 1));
                            if (!strings[index]) {
                                if (isQuotable) {
                                    free(prop2);
                                }
                                outOfMemory(TEXT("WBJCAJA"), 1);
                                index = -1;
                                break;
                            }
                            _tcsncpy(strings[index], prop, len);
                            strings[index][len] = 0;
                            splitShift = len + 1;
                            while (prop[splitShift] == TEXT(' ')) {
                                splitShift++;
                            }
                            prop += splitShift;
                        }
                        index++;
                        if (collectAppOnlyIndexes) {
                            /* The previously calculated scope also applies to the split argument. */
                            if (!(scope & JVM_TARGET_DRYRUN)) {
                                /* This argument is not for the --dry-run invocation. */
                                if (addAppOnlyAdditional(index)) {
                                    index = -1;
                                    break;
                                }
                            }
                        }
                    }
                }

                if (strings) {
                    updateStringValue(&(strings[index]), prop);

                    /* Set if this parameter enables debugging. */
                    if (detectDebugJVM) {
                        if (_tcsstr(strings[index], TEXT("-Xdebug")) == strings[index]) {
                            wrapperData->debugJVM = TRUE;
                        }
                    }
                }

                index++;
            }
            if (isQuotable) {
                free(prop2);
            }
        }
        i++;
    }
    freeStringProperties(propertyNames, propertyValues, propertyIndices);

    /* Store additional java parameters specified in the parameter file */
    if ((index = wrapperBuildFileParameters(strings, wrapperData->additionalFile, target, allowSplit, collectAppOnlyIndexes, index)) < 0) {
        return -1;
    }

    if (wrapperData->additionalFile && wrapperData->additionalFile->hasCipher) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL,
            TEXT("Parameter file specified with property '%s' contains sensitive data.\n  Passing its parameters via the java command line would be insecure and is therefore not allowed."), TEXT("wrapper.app.additional_file"));
        return -1;
    }

    return index;
}

/**
 * Java command line callback.
 *
 * @return FALSE if there were any problems.
 */
static int loadParameterFileCallbackParam_AddArg(LoadParameterFileCallbackParam *param, TCHAR *arg, size_t argLen, const TCHAR *fileName, int expandVars, int minLogLevel) {
    TCHAR *argTerm;
    TCHAR *argUnquoted;
    TCHAR buffer[MAX_PROPERTY_VALUE_LENGTH];
    int* pHasPercent = NULL;
    int hasCipher = FALSE;
    int mode;
    size_t len;

    /* The incoming arg can not be considered to be null terminated so we need a local copy. */
    if (!(argTerm = malloc(sizeof(TCHAR) * (argLen + 1)))) {
        outOfMemory(TEXT("LPFCPAA"), 1);
        return FALSE;
    }
    memcpy(argTerm, arg, sizeof(TCHAR) * argLen);
    argTerm[argLen] = TEXT('\0');
#ifdef _DEBUG
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_NOTICE, TEXT("    :> %s"), argTerm);
#endif

    mode = CIPHER_KEEP;
    if (expandVars) {
        mode |= VAR_EXPAND;
        if (param->quotable) {
            mode |= VAR_ESCAPEQUOTES;
        }
    } else {
        mode |= VAR_KEEP;
    }
    evaluateEnvironmentVariables(argTerm, buffer, MAX_PROPERTY_VALUE_LENGTH, properties->logWarnings, properties->warnedVarMap, properties->logWarningLogLevel, properties->ignoreVarMap, FALSE, pHasPercent, &hasCipher, NULL, mode);

    if (!hasCipher) {
        free(argTerm);
        argTerm = NULL;
    } else {
        wrapperSecureFreeStrW(argTerm);
        argTerm = NULL;

        param->hasCipher |= hasCipher;

        /* Check the file permissions and decipher the data. */
        if (checkFilePermissions(fileName)) {
            /* Do not return FALSE as this would be considered a failure to load the file. */
            wrapperData->insecure = TRUE;
            wrapperSecureZero(buffer, sizeof(buffer));
            return TRUE;
        } else if (wrapperData->cipherPermanentFailure) {
            /* A fatal exception was reported on a previous attempt to decipher.
             *  Do not return FALSE as this would be considered a failure to load this file. */
            wrapperSecureZero(buffer, sizeof(buffer));
            return TRUE;
        } else {
            decipherSensitiveData(fileName, buffer, &argTerm);
            wrapperSecureZero(buffer, sizeof(buffer));
            if (!argTerm) {
                /* We previously checked that the buffer contains ciphers, so argTerm should not be NULL unless there was an unexpected error. */
                return FALSE;
            }
        }
    }

    if (!argTerm) {
        len = _tcslen(buffer);
        argTerm = malloc(sizeof(TCHAR) * (len + 1));
        if (!argTerm) {
            if (hasCipher) {
                wrapperSecureZero(buffer, sizeof(buffer));
            }
            outOfMemory(TEXT("LPFCPAA"), 2);
            return FALSE;
        }
        _tcsncpy(argTerm, buffer, len + 1);

        if (hasCipher) {
            wrapperSecureZero(buffer, sizeof(buffer));
        }
    }

    if (param->quotable) {
        /* Quotes need to be processed. */
        /* Note: In wrapperBuildJavaCommandArrayJavaAdditional(), we call wrapperUnquoteArg() with allowSplit = TRUE,
         *       but this shouldn't be needed here because the arguments have already been split based on quotes and spaces.
         *       A splittable option works if the file is not quotable or if the whole argument is enclosed in quotes. */
        argUnquoted = wrapperUnquoteArg(argTerm, fileName, TRUE, FALSE, minLogLevel);
        if (hasCipher) {
            wrapperSecureFreeStrW(argTerm);
        } else {
            free(argTerm);
        }
        if (!argUnquoted) {
            /* error reported */
            return FALSE;
        }
        argTerm = argUnquoted;
    }

    if (param->isJVMParam) {
        /* As in wrapperBuildJavaCommandArrayJavaAdditional(), skip an argument which does not begin with '-'. */
        if (argTerm[0] != TEXT('-')) {
            /* Java additionals do not contain sensitive data. It is ok to log the value. */
            if (minLogLevel <= LEVEL_FATAL) {
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("The value '%s' is not a valid argument to the JVM."), argTerm);
            }
            if (hasCipher) {
                wrapperSecureFreeStrW(argTerm);
            } else {
                free(argTerm);
            }

            /* Return TRUE to keep checking other args, but keep a flag to remember that the file is invalid. */
            param->hasInvalidArg = TRUE;
            return TRUE;
        }
    } else if (param->isAppProperty) {
        if (!_tcsstr(argTerm, TEXT("="))) {
            if (hasCipher) {
                if (minLogLevel <= LEVEL_FATAL) {
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Invalid system property found (concealed)."));
                }
                wrapperSecureFreeStrW(argTerm);
            } else {
                if (minLogLevel <= LEVEL_FATAL) {
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("The value '%s' is not a valid system property."), argTerm);
                }
                free(argTerm);
            }

            /* Return TRUE to keep checking other args, but keep a flag to remember that the file is invalid. */
            param->hasInvalidArg = TRUE;
            return TRUE;
        }
    }

    if (param->strings) {
        if (!param->isJVMParam) {
            /* Application properties and parameters are for the Java application and Dry-run call. */
            param->scopes[param->index] = JVM_TARGET_DRYRUN_APP;
        } else {
            /* Java additional options are for all JVMs by default, except for certain specific options. */
            param->scopes[param->index] = getRealAdditionalScope(argTerm, JVM_TARGET_ALL, NULL, FALSE);
        }
        param->strings[param->index] = argTerm;
    } else if (hasCipher) {
        wrapperSecureFreeStrW(argTerm);
    } else {
        free(argTerm);
    }
    
    param->index++;
    return TRUE;
}

static int loadParameterFileCallback(void *callbackParam, const TCHAR *fileName, int lineNumber, int depth, TCHAR *config, int expandVars, int minLogLevel) {
    LoadParameterFileCallbackParam *param = (LoadParameterFileCallbackParam *)callbackParam;
    TCHAR *tail_bound;
    TCHAR *arg;
    TCHAR *s;
    size_t len;
    int InDelim = FALSE;
    int InQuotes = FALSE;
    int Escaped = FALSE;

#ifdef _DEBUG
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_NOTICE, TEXT("    : %s"), config);
#endif

    len = _tcslen(config);

    /* Assume that the line `config' has no white spaces at its
       beginning and end. */
    assert(config && len > 0);
    assert(config[0] != TEXT(' ') && config[len - 1] != TEXT(' '));

    if (!param->quotable) {
        if (!loadParameterFileCallbackParam_AddArg(param, config, len, fileName, expandVars, minLogLevel)) {
            return FALSE;
        }
    } else {
        tail_bound = config + len + 1;
        for (arg = s = config; s < tail_bound; s++) {
            switch (*s) {
            case TEXT('\0'):
                if (!loadParameterFileCallbackParam_AddArg(param, arg, s - arg, fileName, expandVars, minLogLevel)) {
                    return FALSE;
                }
                break;
            case TEXT(' '):
            case TEXT('\t'):
                Escaped = FALSE;
                if (!InDelim && !InQuotes) {
                    InDelim = TRUE;
                    if (!loadParameterFileCallbackParam_AddArg(param, arg, s - arg, fileName, expandVars, minLogLevel)) {
                        return FALSE;
                    }
                }
                break;
            case TEXT('"'):
                if (!Escaped) {
                    InQuotes = !InQuotes;
                }
                Escaped = FALSE;
                if (InDelim) {
                    InDelim = FALSE;
                    arg = s;
                }
                break;
            case TEXT('\\'):
                Escaped = !Escaped;
                if (InDelim) {
                    InDelim = FALSE;
                    arg = s;
                }
                break;
            default:
                Escaped = FALSE;
                if (InDelim) {
                    InDelim = FALSE;
                    arg = s;
                }
                break;
            }
        }
    }

    return TRUE;
}

/**
 * Builds up the additional section of the Java command line.
 *
 * @return The final index into the strings array, or -1 if there were any problems.
 */
int wrapperLoadParameterFileInner(TCHAR **strings, int* scopes, const TCHAR *filePath, const TCHAR *propName, int required, int quotable, int isJVMParameter, int isAppProperty, int preload, int* pHasCipher) {
    LoadParameterFileCallbackParam callbackParam;
    int readResult;

    callbackParam.quotable = quotable;
    callbackParam.strings = strings;
    callbackParam.scopes = scopes;
    callbackParam.index = 0;
    callbackParam.isJVMParam = isJVMParameter;
    callbackParam.isAppProperty = isAppProperty;
    callbackParam.hasCipher = FALSE;
    callbackParam.hasInvalidArg = FALSE;

    readResult = configFileReader(filePath, required, loadParameterFileCallback, &callbackParam, NULL, NULL, propName, FALSE, preload, getLoadLogLevel(preload), wrapperData->originalWorkingDir, properties->warnedVarMap, properties->ignoreVarMap, properties->logWarnings, properties->logWarningLogLevel);

    if (pHasCipher) {
        *pHasCipher = callbackParam.hasCipher;
    }
    if (callbackParam.hasInvalidArg) {
        return -1;
    }

    switch (readResult) {
    case CONFIG_FILE_READER_OPEN_FAIL:
        return required ? -1 : 0;
    case CONFIG_FILE_READER_SUCCESS:
        return callbackParam.index;
    case CONFIG_FILE_READER_FAIL:
        return -1;
    default:
        _tprintf(TEXT("Unexpected read error %d\n"), readResult);
        return 0;
    };
}

/**
 * Retrieve the library path from the environment and optionally strip quotes.
 *
 * @param envName     Platform-specific name of the environment variable storing library paths.
 *
 * @return A copy of the environment with quotes optionally stripped.
 *         The returned value must be freed by the caller.
 */
static TCHAR* getLibraryAppendPath(const TCHAR* envName) {
#ifdef WIN32
    int i, j;
#endif
    TCHAR* systemPath;
    TCHAR* result = NULL;

    systemPath = _tgetenv(envName);
    if (systemPath) {
#if !defined(WIN32) && defined(UNICODE)
        /* We already copied the environment table entry in our own implementation of _tgetenv(). */
        result = systemPath;
#else
        /* Work on a copy to not modify the environment table entry. */
        updateStringValue(&result, systemPath);
#endif

        /* We are going to add our own quotes, so we need to make sure that the system
         *  PATH doesn't contain any of its own. */
#ifdef WIN32
        i = 0;
        j = 0;
        do {
            if (result[i] != TEXT('"')) {
                result[j] = result[i];
                j++;
            }
            i++;
        } while (result[j] != TEXT('\0'));
#endif
    }
    return result;
}

/**
 * Builds up the library path section of the Java command line.
 *
 * @return The final index into the strings array, or -1 if there were any problems.
 */
int wrapperBuildJavaCommandArrayLibraryPath(TCHAR **strings, int index) {
    const TCHAR *prop;
    int i, j;
    size_t len2;
    size_t cpLen, cpLenAlloc;
    TCHAR *tmpString;
    TCHAR *systemPath;
#ifdef MACOSX
    size_t len;
    TCHAR *systemPathDY;
    TCHAR *systemPathLD;
#endif
    TCHAR **propertyNames;
    TCHAR **propertyValues;
    long unsigned int *propertyIndices;

    if (strings) {
        if (wrapperData->libraryPathAppendPath) {
            /* We are going to want to append the full system path to
             *  whatever library path is generated. */
#ifdef MACOSX
            /* On macOS, DYLD_LIBRARY_PATH is commonly used to specify directories where the dynamic linker should look
             *  for libraries, but LD_LIBRARY_PATH is also used for compatibility with Unix.
             *  The best would be to combine the paths of the two variables, but this would make the code more complex...
             *  It doesn't hurt to have duplicated paths, so lets append both with priority to DYLD_LIBRARY_PATH. */
            systemPathDY = getLibraryAppendPath(TEXT("DYLD_LIBRARY_PATH"));
            systemPathLD = getLibraryAppendPath(TEXT("LD_LIBRARY_PATH"));
            if (systemPathDY && systemPathLD) {
                len = _tcslen(systemPathDY) + 1 + _tcslen(systemPathLD);
                systemPath = malloc(sizeof(TCHAR) * (len + 1));
                if (!systemPath) {
                    outOfMemory(TEXT("WBJCALP"), 0);
                    free(systemPathDY);
                    free(systemPathLD);
                    return -1;
                }
                _sntprintf(systemPath, len + 1, TEXT("%s:%s"), systemPathDY, systemPathLD);
                free(systemPathDY);
                free(systemPathLD);
            } else if (systemPathDY) {
                systemPath = systemPathDY;
            } else if (systemPathLD) {
                systemPath = systemPathLD;
            } else {
                systemPath = NULL;
            }
#elif defined(WIN32)
            systemPath = getLibraryAppendPath(TEXT("PATH"));
#else
            systemPath = getLibraryAppendPath(TEXT("LD_LIBRARY_PATH"));
#endif
        } else {
            systemPath = NULL;
        }

        prop = getStringProperty(properties, TEXT("wrapper.java.library.path"), NULL);
        if (prop) {
            /* An old style library path was specified.
             * If quotes are being added, check the last character before the
             *  closing quote. If it is a backslash then Windows will use it to
             *  escape the quote.  To make things work correctly, we need to add
             *  another backslash first so it will result in a single backslash
             *  before the quote. */
            if (systemPath) {
                strings[index] = malloc(sizeof(TCHAR) * (22 + _tcslen(prop) + 1 + _tcslen(systemPath) + 1 + 1));
                if (!strings[index]) {
                    outOfMemory(TEXT("WBJCALP"), 1);
                    free(systemPath);
                    return -1;
                }
                _sntprintf(strings[index], 22 + _tcslen(prop) + 1 + _tcslen(systemPath) + 1 + 1, TEXT("-Djava.library.path=%s%c%s"), prop, wrapperClasspathSeparator, systemPath);
            } else {
                strings[index] = malloc(sizeof(TCHAR) * (22 + _tcslen(prop) + 1 + 1));
                if (!strings[index]) {
                    outOfMemory(TEXT("WBJCALP"), 2);
                    return -1;
                }
                _sntprintf(strings[index], 22 + _tcslen(prop) + 1 + 1, TEXT("-Djava.library.path=%s"), prop);
            }
        } else {
            /* Look for a multiline library path. */
            cpLen = 0;
            cpLenAlloc = 1024;
            strings[index] = malloc(sizeof(TCHAR) * cpLenAlloc);
            if (!strings[index]) {
                outOfMemory(TEXT("WBJCALP"), 3);
                if (systemPath) {
                    free(systemPath);
                }
                return -1;
            }

            /* Start with the property value. */
            _sntprintf(&(strings[index][cpLen]), cpLenAlloc - cpLen, TEXT("-Djava.library.path="));
            cpLen += 20;

            /* Loop over the library path entries adding each one */
            if (getStringProperties(properties, TEXT("wrapper.java.library.path."), TEXT(""), wrapperData->ignoreSequenceGaps, FALSE, &propertyNames, &propertyValues, &propertyIndices)) {
                /* Failed */
                if (systemPath) {
                    free(systemPath);
                }
                return -1;
            }

            i = 0;
            j = 0;
            while (propertyNames[i]) {
                prop = propertyValues[i];
                if (prop) {
                    len2 = _tcslen(prop);
                    if (len2 > 0) {
                        /* Is there room for the entry? */
                        while (cpLen + len2 + 3 > cpLenAlloc) {
                            /* Resize the buffer */
                            tmpString = strings[index];
                            cpLenAlloc += 1024;
                            strings[index] = malloc(sizeof(TCHAR) * cpLenAlloc);
                            if (!strings[index]) {
                                outOfMemory(TEXT("WBJCALP"), 4);
                                if (systemPath) {
                                    free(systemPath);
                                }
                                return -1;
                            }
                            _sntprintf(strings[index], cpLenAlloc, TEXT("%s"), tmpString);
                            free(tmpString);
                            tmpString = NULL;
                        }

                        if (j > 0) {
                            strings[index][cpLen++] = wrapperClasspathSeparator; /* separator */
                        }
                        _sntprintf(&(strings[index][cpLen]), cpLenAlloc - cpLen, TEXT("%s"), prop);
                        cpLen += len2;
                        j++;
                    }
                    i++;
                }
            }
            freeStringProperties(propertyNames, propertyValues, propertyIndices);

            if (systemPath) {
                /* We need to append the system path. */
                len2 = _tcslen(systemPath);
                if (len2 > 0) {
                    /* Is there room for the entry? */
                    while (cpLen + len2 + 3 > cpLenAlloc) {
                        /* Resize the buffer */
                        tmpString = strings[index];
                        cpLenAlloc += 1024;
                        strings[index] = malloc(sizeof(TCHAR) * cpLenAlloc);
                        if (!strings[index]) {
                            outOfMemory(TEXT("WBJCALP"), 5);
                            free(systemPath);
                            return -1;
                        }
                        _sntprintf(strings[index], cpLenAlloc, TEXT("%s"), tmpString);
                        free(tmpString);
                        tmpString = NULL;
                    }

                    if (j > 0) {
                        strings[index][cpLen++] = wrapperClasspathSeparator; /* separator */
                    }
                    _sntprintf(&(strings[index][cpLen]), cpLenAlloc - cpLen, TEXT("%s"), systemPath);
                    cpLen += len2;
                    j++;
                }
            }

            if (j == 0) {
                /* No library path, use default. always room */
                _sntprintf(&(strings[index][cpLen]), cpLenAlloc - cpLen, TEXT("./"));
                cpLen++;
            }
        }

        if (systemPath) {
            free(systemPath);
        }
    }
    index++;
    return index;
}

/**
 * Builds up the java classpath.
 *
 * @return 0 if successful, or -1 if there were any problems.
 */
int wrapperBuildJavaClasspath(TCHAR **classpath, int addWrapperJar) {
    TCHAR *prop;
    TCHAR *propBaseDir;
    int i, j;
    size_t cpLen, cpLenAlloc;
    size_t len2;
    TCHAR *tmpString;
#if defined(WIN32) && !defined(WIN64)
    struct _stat64i32 statBuffer;
#else
    struct stat statBuffer;
#endif
    TCHAR **propertyNames;
    TCHAR **propertyValues;
    long unsigned int *propertyIndices;
    TCHAR **files;
    int cnt;
    int missingLogLevel;

    /* Loop over the classpath entries adding each one. */
    if (getStringProperties(properties, TEXT("wrapper.java.classpath."), TEXT(""), wrapperData->ignoreSequenceGaps, FALSE, &propertyNames, &propertyValues, &propertyIndices)) {
        /* Failed */
        return -1;
    }

    /* Build a classpath */
    cpLen = 0;
    cpLenAlloc = 1024;
    if (*classpath) {
        free(*classpath);
    }
    *classpath = malloc(sizeof(TCHAR) * cpLenAlloc);
    if (!*classpath) {
        outOfMemory(TEXT("WBJCP"), 1);
        freeStringProperties(propertyNames, propertyValues, propertyIndices);
        return -1;
    }
    
    /* Get the loglevel to display warnings about missing classpath elements. */
    missingLogLevel = getLogLevelForName(getStringProperty(properties, TEXT("wrapper.java.classpath.missing.loglevel"), TEXT("DEBUG")));

    i = 0;
    j = 0;
    while (propertyNames[i]) {
        prop = removeOuterQuotes(propertyValues[i]);

        len2 = _tcslen(prop);
        if (len2 > 0) {
            /* Does this contain wildcards? */
            if ((_tcsrchr(prop, TEXT('*')) != NULL) || (_tcschr(prop, TEXT('?')) != NULL)) {
                /* Need to do a wildcard search */
                files = loggerFileGetFiles(prop, LOGGER_FILE_SORT_MODE_NAMES_ASC);
                if (!files) {
                    /* Failed */
                    freeStringProperties(propertyNames, propertyValues, propertyIndices);
                    return -1;
                }

                /* Loop over the files. */
                cnt = 0;
                while (files[cnt]) {
                    if (addWrapperJar && (_tcscmp(wrapperData->wrapperJar, files[cnt]) == 0)) {
                        addWrapperJar = FALSE;
                    }

                    len2 = _tcslen(files[cnt]);

                    /* Is there room for the entry? */
                    if (cpLen + len2 + 3 > cpLenAlloc) {
                        /* Resize the buffer */
                        tmpString = *classpath;
                        cpLenAlloc += len2 + 3;
                        *classpath = malloc(sizeof(TCHAR) * cpLenAlloc);
                        if (!*classpath) {
                            loggerFileFreeFiles(files);
                            freeStringProperties(propertyNames, propertyValues, propertyIndices);
                            outOfMemory(TEXT("WBJCP"), 3);
                            return -1;
                        }
                        if (j > 0) {
                            _sntprintf(*classpath, cpLenAlloc, TEXT("%s"), tmpString);
                        }
                        free(tmpString);
                        tmpString = NULL;
                    }

                    if (j > 0) {
                        (*classpath)[cpLen++] = wrapperClasspathSeparator; /* separator */
                    }
                    _sntprintf(&((*classpath)[cpLen]), cpLenAlloc - cpLen, TEXT("%s"), files[cnt]);
                    cpLen += len2;
                    j++;
                    cnt++;
                }
                loggerFileFreeFiles(files);
                if (cnt <= 0) {
                    log_printf(WRAPPER_SOURCE_WRAPPER, missingLogLevel, TEXT(
                        "Classpath element, %s, did not match any files: %s"), propertyNames[i], prop);
                }
            } else {
                /* This classpath entry does not contain any wildcards. */
                if (addWrapperJar && (_tcscmp(wrapperData->wrapperJar, prop) == 0)) {
                    addWrapperJar = FALSE;
                }

                /* If the path element is a directory then we want to strip the trailing slash if it exists. */
                propBaseDir = (TCHAR*)prop;
                if ((prop[_tcslen(prop) - 1] == TEXT('/')) || (prop[_tcslen(prop) - 1] == TEXT('\\'))) {
                    propBaseDir = malloc(sizeof(TCHAR) * _tcslen(prop));
                    if (!propBaseDir) {
                        outOfMemory(TEXT("WBJCP"), 4);
                        freeStringProperties(propertyNames, propertyValues, propertyIndices);
                        return -1;
                    }
                    _tcsncpy(propBaseDir, prop, _tcslen(prop) - 1);
                    propBaseDir[_tcslen(prop) - 1] = TEXT('\0');
                }

                /* See if it exists so we can display a debug warning if it does not. */
                if (_tstat(propBaseDir, &statBuffer)) {
                    /* Encountered an error of some kind. */
                    if ((errno == ENOENT) || (errno == 3)) {
                        log_printf(WRAPPER_SOURCE_WRAPPER, missingLogLevel, TEXT(
                            "Classpath element, %s, does not exist: %s"), propertyNames[i], prop);
                    } else {
                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT(
                            "Unable to get information of classpath element: %s (%s)"),
                            prop, getLastErrorText());
                    }
                } else {
                    /* Got the stat info. */
                }

                /* If we allocated the propBaseDir buffer then free it up. */
                if (propBaseDir != prop) {
                    free(propBaseDir);
                }
                propBaseDir = NULL;

                /* Is there room for the entry? */
                if (cpLen + len2 + 3 > cpLenAlloc) {
                    /* Resize the buffer */
                    tmpString = *classpath;
                    cpLenAlloc += len2 + 3;
                    *classpath = malloc(sizeof(TCHAR) * cpLenAlloc);
                    if (!*classpath) {
                        outOfMemory(TEXT("WBJCP"), 5);
                        freeStringProperties(propertyNames, propertyValues, propertyIndices);
                        return -1;
                    }
                    if (j > 0) {
                        _sntprintf(*classpath, cpLenAlloc, TEXT("%s"), tmpString);
                    }
                    free(tmpString);
                    tmpString = NULL;
                }

                if (j > 0) {
                    (*classpath)[cpLen++] = wrapperClasspathSeparator; /* separator */
                }
                _sntprintf(&((*classpath)[cpLen]), cpLenAlloc - cpLen, TEXT("%s"), prop);
                cpLen += len2;
                j++;
            }
        }

        i++;
    }
    freeStringProperties(propertyNames, propertyValues, propertyIndices);
    if (j == 0) {
        if (addWrapperJar) {
            updateStringValue(classpath, wrapperData->wrapperJar);
        } else {
            updateStringValue(classpath, TEXT("./"));
        }
    } else {
        if (addWrapperJar) {
            cpLen += _tcslen(wrapperData->wrapperJar) + 1;
            tmpString = malloc(sizeof(TCHAR) * (cpLen + 1));
            if (!tmpString) {
                outOfMemory(TEXT("WBJCP"), 6);
            }
            _sntprintf(tmpString, cpLen + 1, TEXT("%s%c%s"), wrapperData->wrapperJar, wrapperClasspathSeparator, *classpath);
            free(*classpath);
            *classpath = tmpString;
        }
        (*classpath)[cpLen] = 0;
    }

    /* TODO: No longer allow classpath option to be used together with wrapper.java.classpath.<n>. */

    return 0;
}

static int wrapperBuildJavaCommandArrayKeyValueOption(TCHAR **strings, int index, const TCHAR *key, const TCHAR *value) {
    size_t len;

    if (value && (value[0] != 0)) {
        if (strings) {
            len = _tcslen(key);
            strings[index] = malloc(sizeof(TCHAR) * (len + 1));
            if (!strings[index]) {
                outOfMemory(TEXT("WBJCAKVO"), 1);
                return -1;
            }
            _sntprintf(strings[index], len + 1, key);
        }
        index++;
        if (strings) {
            len = _tcslen(value);
            strings[index] = malloc(sizeof(TCHAR) * (len + 1));
            if (!strings[index]) {
                outOfMemory(TEXT("WBJCAKVO"), 2);
                return -1;
            }
            _sntprintf(strings[index], len + 1, TEXT("%s"), value);
        }
        index++;
    }
    return index;
}

/**
 * Builds up the java classpath section of the Java command line.
 *
 * @return The final index into the strings array, or -1 if there were any problems.
 */
int wrapperBuildJavaCommandArrayClasspath(TCHAR **strings, int index, const TCHAR *classpath) {
    return wrapperBuildJavaCommandArrayKeyValueOption(strings, index, TEXT("-classpath"), classpath);
}

int wrapperBuildJavaModulepath(TCHAR** modulepath, int addWrapperJar, int isUpgrade, int *pUserDefined) {
    const TCHAR *propNameBase;
    const TCHAR *prop;
    TCHAR *propBaseDir;
    int i, j;
    size_t mpLen, mpLenAlloc;
    size_t len2;
    TCHAR *tmpString;
#if defined(WIN32) && !defined(WIN64)
    struct _stat64i32 statBuffer;
#else
    struct stat statBuffer;
#endif
    TCHAR **propertyNames;
    TCHAR **propertyValues;
    long unsigned int *propertyIndices;
    TCHAR **files;
    int cnt;
    int missingLogLevel;

    /* Loop over the upgrade module path entries adding each one. */
    propNameBase = isUpgrade ? TEXT("wrapper.java.upgrade_module_path.") : TEXT("wrapper.java.module_path.");
    if (getStringProperties(properties, propNameBase, TEXT(""), wrapperData->ignoreSequenceGaps, FALSE, &propertyNames, &propertyValues, &propertyIndices)) {
        /* Failed */
        return -1;
    }

    /* Build a modulepath */
    mpLen = 0;
    mpLenAlloc = 1024;
    if (*modulepath) {
        free(*modulepath);
    }
    *modulepath = malloc(sizeof(TCHAR) * mpLenAlloc);
    if (!*modulepath) {
        outOfMemory(TEXT("WBJMP"), 1);
        freeStringProperties(propertyNames, propertyValues, propertyIndices);
        return -1;
    }

    /* Get the loglevel to display warnings about missing modulepath elements. */
    prop = isUpgrade ? TEXT("wrapper.java.upgrade_module_path.missing.loglevel") : TEXT("wrapper.java.module_path.missing.loglevel");
    missingLogLevel = getLogLevelForName(getStringProperty(properties, prop, TEXT("DEBUG")));

    i = 0;
    j = 0;
    while (propertyNames[i]) {
        prop = removeOuterQuotes(propertyValues[i]);

        len2 = _tcslen(prop);
        if (len2 > 0) {
            /* Does this contain wildcards? */
            if ((_tcsrchr(prop, TEXT('*')) != NULL) || (_tcschr(prop, TEXT('?')) != NULL)) {
                /* Need to do a wildcard search */
                files = loggerFileGetFiles(prop, LOGGER_FILE_SORT_MODE_NAMES_ASC);
                if (!files) {
                    /* Failed */
                    freeStringProperties(propertyNames, propertyValues, propertyIndices);
                    return -1;
                }

                /* Loop over the files. */
                cnt = 0;
                while (files[cnt]) {
                    if (!isUpgrade) {
                        if (wrapperData->wrapperJar && _tcscmp(wrapperData->wrapperJar, files[cnt]) == 0) {
                            /* Skip as it will be added in the upgrade-module-path (or exists in the runtime image). */
                            cnt++;
                            continue;
                        }
                    } else {
                        /* If specified keep it in the same position, otherwise it will be added in the beginning. */
                        if (addWrapperJar && _tcscmp(wrapperData->wrapperJar, files[cnt]) == 0) {
                            addWrapperJar = FALSE;
                        }
                    }

                    len2 = _tcslen(files[cnt]);

                    /* Is there room for the entry? */
                    if (mpLen + len2 + 3 > mpLenAlloc) {
                        /* Resize the buffer */
                        tmpString = *modulepath;
                        mpLenAlloc += len2 + 3;
                        *modulepath = malloc(sizeof(TCHAR) * mpLenAlloc);
                        if (!*modulepath) {
                            loggerFileFreeFiles(files);
                            freeStringProperties(propertyNames, propertyValues, propertyIndices);
                            outOfMemory(TEXT("WBJCP"), 2);
                            return -1;
                        }
                        if (j > 0) {
                            _sntprintf(*modulepath, mpLenAlloc, TEXT("%s"), tmpString);
                        }
                        free(tmpString);
                        tmpString = NULL;
                    }

                    if (j > 0) {
                        (*modulepath)[mpLen++] = wrapperClasspathSeparator; /* separator */
                    }
                    _sntprintf(&((*modulepath)[mpLen]), mpLenAlloc - mpLen, TEXT("%s"), files[cnt]);
                    mpLen += len2;
                    j++;
                    cnt++;
                }
                loggerFileFreeFiles(files);
                if (cnt <= 0) {
                    log_printf(WRAPPER_SOURCE_WRAPPER, missingLogLevel, TEXT(
                        "Modulepath element, %s, did not match any files: %s"), propertyNames[i], prop);
                }
            } else {
                if (!isUpgrade) {
                    if (wrapperData->wrapperJar && _tcscmp(wrapperData->wrapperJar, prop) == 0) {
                        /* Skip as it will be added in the upgrade-module-path (or exists in the runtime image). */
                        i++;
                        continue;
                    }
                } else {
                    /* If specified keep it in the same position, otherwise it will be added in the beginning. */
                    if (addWrapperJar && _tcscmp(wrapperData->wrapperJar, prop) == 0) {
                        addWrapperJar = FALSE;
                    }
                }

                /* Strip the trailing slash if it exists. */
                propBaseDir = (TCHAR*)prop;
                if ((prop[_tcslen(prop) - 1] == TEXT('/')) || (prop[_tcslen(prop) - 1] == TEXT('\\'))) {
                    propBaseDir = malloc(sizeof(TCHAR) * _tcslen(prop));
                    if (!propBaseDir) {
                        outOfMemory(TEXT("WBJMP"), 3);
                        freeStringProperties(propertyNames, propertyValues, propertyIndices);
                        return -1;
                    }
                    _tcsncpy(propBaseDir, prop, _tcslen(prop) - 1);
                    propBaseDir[_tcslen(prop) - 1] = TEXT('\0');
                }

                /* See if it exists so we can display a debug warning if it does not. */
                if (_tstat(propBaseDir, &statBuffer)) {
                    /* Encountered an error of some kind. */
                    if ((errno == ENOENT) || (errno == 3)) {
                        log_printf(WRAPPER_SOURCE_WRAPPER, missingLogLevel, TEXT(
                            "Module path element, %s, does not exist: %s"), propertyNames[i], prop);
                    } else {
                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT(
                            "Unable to get information of module path element: %s (%s)"),
                            prop, getLastErrorText());
                    }
                } else {
                    /* Got the stat info. */
                }

                /* If we allocated the propBaseDir buffer then free it up. */
                if (propBaseDir != prop) {
                    free(propBaseDir);
                }
                propBaseDir = NULL;

                /* Is there room for the entry? */
                if (mpLen + len2 + 3 > mpLenAlloc) {
                    /* Resize the buffer */
                    tmpString = *modulepath;
                    mpLenAlloc += len2 + 3;
                    *modulepath = malloc(sizeof(TCHAR) * mpLenAlloc);
                    if (!*modulepath) {
                        outOfMemory(TEXT("WBJMP"), 4);
                        freeStringProperties(propertyNames, propertyValues, propertyIndices);
                        return -1;
                    }
                    if (j > 0) {
                        _sntprintf(*modulepath, mpLenAlloc, TEXT("%s"), tmpString);
                    }
                    free(tmpString);
                    tmpString = NULL;
                }

                if (j > 0) {
                    (*modulepath)[mpLen++] = wrapperClasspathSeparator; /* separator */
                }
                _sntprintf(&((*modulepath)[mpLen]), mpLenAlloc - mpLen, TEXT("%s"), prop);
                mpLen += len2;
                j++;
            }
        }

        i++;
    }
    freeStringProperties(propertyNames, propertyValues, propertyIndices);
    if (j == 0) {
        free(*modulepath);
        if (addWrapperJar) {
            mpLen = _tcslen(wrapperData->wrapperJar);
            tmpString = malloc(sizeof(TCHAR) * (mpLen + 1));
            if (!tmpString) {
                outOfMemory(TEXT("WBJMP"), 5);
            }
            _sntprintf(tmpString, mpLen + 1, TEXT("%s"), wrapperData->wrapperJar);
            *modulepath = tmpString;
        } else {
            *modulepath = NULL;
        }
    } else {
        if (pUserDefined) {
            *pUserDefined = TRUE;
        }

        /* Do not allow to use wrapper.java.*module_path.<n> in combination with the --*module-path option. */
        if (isUpgrade && isInJavaAdditionals(TEXT("--upgrade-module-path"), NULL)) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("%s properties cannot be used when the %s option is also specified."), TEXT("wrapper.java.upgrade_module_path.<n>"), TEXT("--upgrade-module-path"));
            return -1;
        } else if (!isUpgrade && (isInJavaAdditionals(TEXT("--module-path"), NULL) || isInJavaAdditionals(TEXT("-p"), NULL))) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("%s properties cannot be used when the %s or %s options are also specified."), TEXT("wrapper.java.module_path.<n>"), TEXT("--module-path"), TEXT("-p"));
            return -1;
        }
        if (addWrapperJar) {
            mpLen += _tcslen(wrapperData->wrapperJar) + 1;
            tmpString = malloc(sizeof(TCHAR) * (mpLen + 1));
            if (!tmpString) {
                outOfMemory(TEXT("WBJMP"), 6);
            }
            _sntprintf(tmpString, mpLen + 1, TEXT("%s%c%s"), wrapperData->wrapperJar, wrapperClasspathSeparator, *modulepath);
            free(*modulepath);
            *modulepath = tmpString;
        }
    }

    return 0;
}

/**
 * Builds up the --module-path section of the Java command line.
 *
 * @return The final index into the strings array, or -1 if there were any problems.
 */
int wrapperBuildJavaCommandArrayModulepath(TCHAR **strings, int index, const TCHAR *modulePath) {
    return wrapperBuildJavaCommandArrayKeyValueOption(strings, index, TEXT("--module-path"), modulePath);
}

/**
 * Builds up the java --upgrade-module-path section of the Java command line.
 *
 * @return The final index into the strings array, or -1 if there were any problems.
 */
int wrapperBuildJavaCommandArrayUpgradeModulepath(TCHAR **strings, int index, const TCHAR *upgradeModulePath) {
    return wrapperBuildJavaCommandArrayKeyValueOption(strings, index, TEXT("--upgrade-module-path"), upgradeModulePath);
}

int wrapperBuildJavaModulelist(TCHAR** modulelist, TCHAR** nativeAccessModulelist) {
    TCHAR *prop;
    int isWrapperModule;
    int wrapperModuleFound = FALSE;
    int nativeAccessEnabled;
    int nativeAccessEnabledConfigured = FALSE;
    int i, j, k;
    size_t mlLen, mlLenAlloc, namlLen = 0, namlLenAlloc = 0;
    size_t len2;
    TCHAR buffer[128];
    TCHAR *tmpString;
    TCHAR **propertyNames;
    TCHAR **propertyValues;
    long unsigned int *propertyIndices;

    /* Loop over the module list entries adding each one. */
    if (getStringProperties(properties, TEXT("wrapper.java.module."), TEXT(""), wrapperData->ignoreSequenceGaps, FALSE, &propertyNames, &propertyValues, &propertyIndices)) {
        /* Failed */
        return -1;
    }

    /* Build a modulelist */
    mlLen = 0;
    mlLenAlloc = 1024;
    if (*modulelist) {
        free(*modulelist);
    }
    *modulelist = malloc(sizeof(TCHAR) * mlLenAlloc);
    if (!*modulelist) {
        outOfMemory(TEXT("WBJML"), 1);
        freeStringProperties(propertyNames, propertyValues, propertyIndices);
        return -1;
    }
    if (nativeAccessModulelist) {
        namlLen = 0;
        namlLenAlloc = 1024;
        if (*nativeAccessModulelist) {
            free(*nativeAccessModulelist);
        }
        *nativeAccessModulelist = malloc(sizeof(TCHAR) * namlLenAlloc);
        if (!*nativeAccessModulelist) {
            outOfMemory(TEXT("WBJML"), 2);
            freeStringProperties(propertyNames, propertyValues, propertyIndices);
            free(*modulelist);
            return -1;
        }
    }

    i = 0;
    j = 0;
    k = 0;
    while (propertyNames[i]) {
        prop = removeOuterQuotes(propertyValues[i]);

        len2 = _tcslen(prop);
        if (len2 > 0) {
            if (_tcschr(prop, TEXT(' ')) || _tcschr(prop, TEXT('\t'))) {
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT(
                    "Module element, %s, contains a space: '%s'. This is not permitted."), propertyNames[i], prop);
                freeStringProperties(propertyNames, propertyValues, propertyIndices);
                return -1;
            }

            /* Is there room for the entry? */
            if (mlLen + len2 + 3 > mlLenAlloc) {
                /* Resize the buffer */
                tmpString = *modulelist;
                mlLenAlloc += len2 + 3;
                *modulelist = malloc(sizeof(TCHAR) * mlLenAlloc);
                if (!*modulelist) {
                    outOfMemory(TEXT("WBJML"), 3);
                    freeStringProperties(propertyNames, propertyValues, propertyIndices);
                    free(tmpString);
                    return -1;
                }
                if (j > 0) {
                    _sntprintf(*modulelist, mlLenAlloc, TEXT("%s"), tmpString);
                }
                free(tmpString);
                tmpString = NULL;
            }

            if (j > 0) {
                (*modulelist)[mlLen++] = TEXT(','); /* separator */
            }
            _sntprintf(&((*modulelist)[mlLen]), mlLenAlloc - mlLen, TEXT("%s"), prop);
            mlLen += len2;
            j++;

            isWrapperModule = (_tcscmp(TEXT("org.tanukisoftware.wrapper"), prop) == 0);
            if (isWrapperModule && !wrapperModuleFound) {
                wrapperModuleFound = TRUE;
            }

            /* Build the list of modules allowed to access native code. This is only necessary for Java 24 onwards. */
            if (nativeAccessModulelist) {
                if (isWrapperModule) {
                    nativeAccessEnabled = TRUE;
                } else {
                    _sntprintf(buffer, 128, TEXT("wrapper.java.module.%lu.enable_native_access"), propertyIndices[i]);
                    nativeAccessEnabled = getBooleanProperty(properties, buffer, FALSE);
                    nativeAccessEnabledConfigured = TRUE;
                }

                if (nativeAccessEnabled) {
                    /* Is there room for the entry? */
                    if (namlLen + len2 + 3 > namlLenAlloc) {
                        /* Resize the buffer */
                        tmpString = *nativeAccessModulelist;
                        namlLenAlloc += len2 + 3;
                        *nativeAccessModulelist = malloc(sizeof(TCHAR) * namlLenAlloc);
                        if (!*nativeAccessModulelist) {
                            outOfMemory(TEXT("WBJML"), 4);
                            freeStringProperties(propertyNames, propertyValues, propertyIndices);
                            free(tmpString);
                            free(*modulelist);
                            return -1;
                        }
                        if (k > 0) {
                            _sntprintf(*nativeAccessModulelist, namlLenAlloc, TEXT("%s"), tmpString);
                        }
                        free(tmpString);
                        tmpString = NULL;
                    }

                    if (k > 0) {
                        (*nativeAccessModulelist)[namlLen++] = TEXT(','); /* separator */
                    }
                    _sntprintf(&((*nativeAccessModulelist)[namlLen]), namlLenAlloc - namlLen, TEXT("%s"), prop);
                    namlLen += len2;
                    k++;
                }
            }
        }

        i++;
    }
    freeStringProperties(propertyNames, propertyValues, propertyIndices);
    if (j == 0) {
        free(*modulelist);
        *modulelist = NULL;

        if (nativeAccessModulelist) {
            free(*nativeAccessModulelist);
            *nativeAccessModulelist = NULL;
        }
    } else {
        /* Do not allow to use wrapper.java.module.<n> in combination with the --add-modules option. */
        if (isInJavaAdditionals(TEXT("--add-modules"), NULL)) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("%s properties cannot be used when the %s option is also specified."), TEXT("wrapper.java.module.<n>"), TEXT("--add-modules"));
            return -1;
        }
        (*modulelist)[mlLen] = 0;

        if (nativeAccessModulelist) {
            if (k == 0) {
                free(*nativeAccessModulelist);
                *nativeAccessModulelist = NULL;
            } else if (nativeAccessEnabledConfigured) {
                /* Do not allow to use wrapper.java.module.<n>.enable_native_access in combination with the --enable-native-access option. */
                if (isInJavaAdditionals(TEXT("--enable-native-access"), NULL)) {
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("%s properties cannot be used when the %s option is also specified."), TEXT("wrapper.java.module.<n>.enable_native_access"), TEXT("--enable-native-access"));
                    return -1;
                }
                (*nativeAccessModulelist)[namlLen] = 0;
            }
        }
    }

    if (nativeAccessModulelist && !wrapperModuleFound) {
        wrapperData->addWrapperToNativeAccessModuleList = TRUE;
    }

    return 0;
}

/**
 * Builds up the --add-modules section of the Java command line.
 *
 * @return The final index into the strings array, or -1 if there were any problems.
 */
int wrapperBuildJavaCommandArrayModulelist(TCHAR **strings, int index, const TCHAR *modulelist) {
    return wrapperBuildJavaCommandArrayKeyValueOption(strings, index, TEXT("--add-modules"), modulelist);
}

int wrapperBuildJavaCommandArrayNativeAccesslist(TCHAR **strings, int index, const TCHAR *modulelist, int addWrapperModule) {
    size_t len;
    TCHAR* newModuleList;
    int result;

    if (addWrapperModule) {
        if (!modulelist || (modulelist[0] == 0)) {
            len = 26;
            newModuleList = malloc(sizeof(TCHAR) * (len + 1));
            if (!newModuleList) {
                outOfMemory(TEXT("WBJCANAL"), 1);
                return -1;
            }
            _tcsncpy(newModuleList, TEXT("org.tanukisoftware.wrapper"), len + 1);
        } else {
            len = 27 + _tcslen(modulelist);
            newModuleList = malloc(sizeof(TCHAR) * (len + 1));
            if (!newModuleList) {
                outOfMemory(TEXT("WBJCANAL"), 2);
                return -1;
            }
            _sntprintf(newModuleList, len + 1, TEXT("org.tanukisoftware.wrapper,%s"), modulelist);
        }
    } else {
        newModuleList = (TCHAR*)modulelist;
    }
    result = wrapperBuildJavaCommandArrayKeyValueOption(strings, index, TEXT("--enable-native-access"), newModuleList);
    if (addWrapperModule) {
        free(newModuleList);
    }
    return result;
}

void wrapperFreeAppParameterArray() {
    if (appParameters) {
        wrapperSecureFreeStringArray(appParameters, appParametersLen);
        appParameters = NULL;
        appParametersLen = 0;
    }
}

int wrapperBuildAppParameterArray() {
    wrapperFreeAppParameterArray();

    if ((appParametersLen = wrapperBuildAppParameterArrayInner(NULL, 0, wrapperData->thisIsTestWrapper, TRUE)) < 0) {
        appParametersLen = 0;
        return TRUE;
    } else if (appParametersLen > 0) {
        appParameters = malloc(sizeof(TCHAR*) * appParametersLen);
        if (!appParameters) {
            outOfMemory(TEXT("WBAPAA"), 1);
            appParametersLen = 0;
            return TRUE;
        } else {
            memset(appParameters, 0, sizeof(TCHAR*) * appParametersLen);
            if (wrapperBuildAppParameterArrayInner(appParameters, 0, wrapperData->thisIsTestWrapper, TRUE) < 0) {
                wrapperFreeAppParameterArray();
                return TRUE;
            }
        }
    }
    return FALSE;
}

void wrapperFreeAppPropertyArray() {
    if (appProperties) {
        wrapperSecureFreeStringArray(appProperties, appPropertiesLen);
        appProperties = NULL;
        appPropertiesLen = 0;
    }
}

int wrapperBuildAppPropertyArray() {
    wrapperFreeAppPropertyArray();

    if ((appPropertiesLen = wrapperBuildAppPropertyArrayInner(NULL)) < 0) {
        appPropertiesLen = 0;
        return TRUE;
    } else if (appPropertiesLen > 0) {
        appProperties = malloc(sizeof(TCHAR*) * appPropertiesLen);
        if (!appProperties) {
            outOfMemory(TEXT("WBAPRA"), 1);
            appPropertiesLen = 0;
            return TRUE;
        } else {
            memset(appProperties, 0, sizeof(TCHAR*) * appPropertiesLen);
            if ((wrapperBuildAppPropertyArrayInner(appProperties)) < 0) {
                wrapperFreeAppPropertyArray();
                return TRUE;
            }
        }
    }
    return FALSE;
}

int isInAppProperties(const TCHAR* propName) {
    int i;
    int ret = FALSE;
    size_t len;
    TCHAR* propValue;

    for (i = 0; i < appPropertiesLen; i++) {
        propValue = appProperties[i];
        if (_tcsstr(propValue, propName) == propValue) {
            /* Make sure that this is the exact property name, not a substring. */
            len = _tcslen(propName);
            if (propValue[len] == TEXT('=')) {
                ret = TRUE;
            }
        }
    }
    return ret;
}

void wrapperFreeJavaAdditionalArray() {
    if (additionals) {
        wrapperFreeStringArray(additionals, additionalsLen);
        additionals = NULL;
        additionalsLen = 0;
    }
}

int wrapperBuildJavaAdditionalArray() {
    wrapperFreeJavaAdditionalArray();

    /* Do not split option when building the internal additionals array as we want to keep the options names and their corresponding values together. */
    if ((additionalsLen = wrapperBuildJavaCommandArrayJavaAdditional(NULL, FALSE, JVM_TARGET_ALL, FALSE, 0)) < 0) {
        additionalsLen = 0;
        return TRUE;
    } else if (additionalsLen > 0) {
        additionals = malloc(sizeof(TCHAR*) * additionalsLen);
        if (!additionals) {
            outOfMemory(TEXT("WBJAA"), 1);
            additionalsLen = 0;
            return TRUE;
        } else {
            memset(additionals, 0, sizeof(TCHAR*) * additionalsLen);
            if (wrapperBuildJavaCommandArrayJavaAdditional(additionals, FALSE, JVM_TARGET_ALL, FALSE, 0) < 0) {
                wrapperFreeJavaAdditionalArray();
                return TRUE;
            }
        }
    }
    return FALSE;
}

/**
 *  Checks the additional java parameters to see if the user has specified a specific system property.
 *   Returns TRUE if already set or there was an error, FALSE if it is safe to set again.
 *
 *  @param propName the name of the system property to search (the length should be less than 32 chars)
 *
 *  @return  TRUE if a JVM option was already specified by the user, FALSE otherwise
 */
int isInJavaAdditionals(const TCHAR* propName, TCHAR** pPropValue) {
    int i;
    int ret = FALSE;
    size_t len;
    TCHAR* propValue;
    TCHAR c;

    for (i = 0; i < additionalsLen; i++) {
        propValue = additionals[i];
        if (_tcsstr(propValue, propName) == propValue) {
            /* Make sure that this is the exact property name, not a substring. */
            len = _tcslen(propName);
            c = propValue[len];
            if (c == TEXT('\0')) {
                ret = TRUE;
            } else if (c == TEXT('=') || (c == TEXT(' ') && !(propName[0] == TEXT('-') && propName[1] == TEXT('D')))) { /* standard java options (not system properties) can have their values specified with a space or a '=' */
                ret = TRUE;
                if (pPropValue) {
                    updateStringValue(pPropValue, propValue + len + 1);
                }
            }
        }
    }
    return ret;
}

int isBackendProperty(const TCHAR* arg) {
    if (((_tcsncmp(arg, TEXT("-Dwrapper.key="), 14) == 0)) ||
        ((_tcsncmp(arg, TEXT("-Dwrapper.port"), 14) == 0)) ||
        ((_tcsncmp(arg, TEXT("-Dwrapper.jvm.port"), 18) == 0)) ||
#ifndef WIN32
        ((_tcsncmp(arg, TEXT("-Dwrapper.pipe."), 15) == 0)) ||
#endif
        ((_tcsncmp(arg, TEXT("-Dwrapper.backend"), 17) == 0))) {
        return TRUE;
    }
    return FALSE;
}

/**
 * Loops over and stores all necessary commands into an array which
 *  can be used to launch a process.
 * This method will only count the elements if stringsPtr is NULL.
 *
 * Note - Next Out Of Memory is #47
 *
 * @return The final index into the strings array, or -1 if there were any problems.
 */
static int wrapperBuildJavaCommandArrayInner(TCHAR **strings, const TCHAR *classpath) {
    int index;
    int detectDebugJVM;
    const TCHAR *prop;
    int initMemory = 0;
    int maxMemory;
    TCHAR encodingBuff[ENCODING_BUFFER_SIZE];
    TCHAR* fileEncodingPtr = NULL;
    size_t moduleLen, packageLen;
    int passEncoding = FALSE;
#ifndef WIN32
    TCHAR localeEncodingBuff[ENCODING_BUFFER_SIZE];
#endif
#ifdef HPUX
    const TCHAR* fix_iconv_hpux;
#endif

    setLogPropertyWarnings(properties, strings != NULL);
    index = 0;

    detectDebugJVM = getBooleanProperty(properties, TEXT("wrapper.java.detect_debug_jvm"), TRUE);

    /* Java command */
    if ((index = wrapperBuildJavaCommandArrayJavaCommand(strings, index, FALSE)) < 0) {
        return -1;
    }
    if (strings && detectDebugJVM) {
        if (isCommandJdb(strings[0])) {
            /* The Java command is the Jdb debugger.  go into debug JVM mode. */
            wrapperData->debugJVM = TRUE;
        }
    }

    /* See if the auto bits parameter is set.  Ignored by all but the following platforms. */

#if /*defined(WIN32) || defined(LINUX) ||*/ defined(HPUX) || defined(MACOSX) || defined(SOLARIS) || defined(FREEBSD)

    if (wrapperData->javaVersion->major < 9) {
        if (getBooleanProperty(properties,
/*#ifdef WIN32
                              TEXT("wrapper.java.additional.auto_bits.windows"),
#elif defined(LINUX)
                              TEXT("wrapper.java.additional.auto_bits.linux"),
*/
#if defined(HPUX)
                              TEXT("wrapper.java.additional.auto_bits.hpux"),
#elif defined(SOLARIS)
                              TEXT("wrapper.java.additional.auto_bits.solaris"),
#elif defined(FREEBSD)
                              TEXT("wrapper.java.additional.auto_bits.freebsd"),
#elif defined(MACOSX)
                              TEXT("wrapper.java.additional.auto_bits.macosx"),
#endif
                              getBooleanProperty(properties, TEXT("wrapper.java.additional.auto_bits"), FALSE))) {
            if (strings) {
                strings[index] = malloc(sizeof(TCHAR) * 5);
                if (!strings[index]) {
                    outOfMemory(TEXT("WBJCAI"), 1);
                    return -1;
                }
                _sntprintf(strings[index], 5, TEXT("-d%s"), wrapperBits);
            }
            index++;
        }
    }
#endif

    /* Store additional java parameters */
    if ((index = wrapperBuildJavaCommandArrayJavaAdditional(strings, detectDebugJVM, JVM_TARGET_DRYRUN_APP, TRUE, index)) < 0) {
        return -1;
    }

#if defined(WIN32) || defined(MACOSX)
    /* On these platforms, the encoding used by the JVM can vary depending on the Java version.
     *  In order to ensure that we read the JVM output correctly and to be consistent across JVMs, we can force the encoding
     *  by setting 'file.encoding'. However, only do this if it was not specified in the Java additional properties. */
    if (!passEncoding && !isInJavaAdditionals(TEXT("-Dfile.encoding"), NULL)) {
 #ifdef WIN32
        if (getBooleanProperty(properties, TEXT("wrapper.java.pass_encoding"), TRUE)) {
            passEncoding = TRUE;
        }
 #endif
        if (!passEncoding && (wrapperData->javaVersion->major < 7)) {
            /* On MacOSX, Java 6 ignores the encoding of the current locale and set file.encoding to "MacRoman".
             * On Windows, Java 6 does not always use the default ANSI Windows code page (for example when the UI language is English but the System language is Japanese). */
            passEncoding = TRUE;
        }
    }
#endif

    if (passEncoding) {
        if (strings) {
            encodingBuff[0] = 0;
#ifdef WIN32
            /* On Windows, convert the code page to the corresponding encoding expressed in the Java syntax: */
            if (!getJvmIoEncodingFromCodePage(wrapperData->jvm_stdout_codepage, wrapperData->javaVersion->major, encodingBuff)) {
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Could not infer the JVM encoding from the code page '%d'."), wrapperData->jvm_stdout_codepage);
#else
            /* On Unix the JVM automatically maps system encodings to its own encodings if nothing is specified in the command line (see note above).
             *  However, when adding system properties we need to convert the locale encoding to the Java syntax: */
            if (!getJvmIoEncoding(getCurrentLocaleEncoding(localeEncodingBuff), wrapperData->javaVersion->major, encodingBuff)) {
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Could not infer the JVM encoding from the locale encoding '%s'."), localeEncodingBuff);
#endif
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ADVICE, TEXT(" Please add the 'file.encoding' system property to the JVM arguments\n and set it with an appropriate encoding for the current platform."));
                /* Stop the Wrapper. The user must fix the configuration. */
                return -1;
            }
            if (!fileEncodingPtr) {
                fileEncodingPtr = encodingBuff;
            }
            /* NOTE: On Linux and jre 1.4.2_19, file.encoding is not used for stdout, but it is used with jre 1.5!
             *       On Solaris and jre 1.4.1_07, file.encoding is used for stdout.
             *       => file.encoding is not a standard specification. Its implementation may differ on old JVMs.
             *       Currently the Wrapper will assume file.encoding is used to encode the JVM output as this is the case for recent JVMs. */
            strings[index] = malloc(sizeof(TCHAR) * (16 + _tcslen(fileEncodingPtr) + 1));
            if (!strings[index]) {
                outOfMemory(TEXT("WBJCAI"), 4);
                return -1;
            }
            _sntprintf(strings[index], 16 + _tcslen(fileEncodingPtr) + 1, TEXT("-Dfile.encoding=%s"), fileEncodingPtr);
        }
        index++;
    }
    
    if (wrapperData->use_sun_encoding) {
        /* On the Java side, we need to know if sun.stdout.encoding (and sun.stderr.encoding) will be used to read JVM outputs.
         *  There is indeed a chance that these properties are passed to the JVM although they are not supported, and we don't
         *  want to remove any system property that the user may have added. We could duplicate the logic on the Java side to
         *  check whether or not these properties are implemented, but there is a risk of not being in sync. Instead we will
         *  pass a system property to inform the JVM when sun.stdout.encoding is used. This assumes resolveJvmEncoding() was
         *  called earlier. */
        if (strings) {
            strings[index] = malloc(sizeof(TCHAR) * (31 + 1));
            if (!strings[index]) {
                outOfMemory(TEXT("WBJCAI"), 7);
                return -1;
            }
            _sntprintf(strings[index], 31 + 1, TEXT("-Dwrapper.use_sun_encoding=true"));
        }
        index++;
    }

#ifdef HPUX
    fix_iconv_hpux = getStringProperty(properties, TEXT("wrapper.fix_iconv_hpux"), NULL);
    if (fix_iconv_hpux) {
        if (strings) {
            strings[index] = malloc(sizeof(TCHAR) * (25 + _tcslen(fix_iconv_hpux) + 1));
            if (!strings[index]) {
                outOfMemory(TEXT("WBJCAI"), 9);
                return -1;
            }
            _sntprintf(strings[index], 25 + _tcslen(fix_iconv_hpux) + 1, TEXT("-Dwrapper.fix_iconv_hpux=%s"), fix_iconv_hpux);
        }
        index++;
    }
#endif

    /* Initial JVM memory */
    initMemory = getIntProperty(properties, TEXT("wrapper.java.initmemory"), 0);
    if (initMemory > 0) {
        if (strings) {
            initMemory = __max(initMemory, 1); /* 1 <= n */
            strings[index] = malloc(sizeof(TCHAR) * (5 + 10 + 1));  /* Allow up to 10 digits. */
            if (!strings[index]) {
                outOfMemory(TEXT("WBJCAI"), 10);
                return -1;
            }
            _sntprintf(strings[index], 5 + 10 + 1, TEXT("-Xms%dm"), initMemory);
        }
        index++;
    } else {
            /* Set the initMemory so the checks in the maxMemory section below will work correctly. */
            initMemory = 3;
    }

    /* Maximum JVM memory */
    maxMemory = getIntProperty(properties, TEXT("wrapper.java.maxmemory"), 0);
    if (maxMemory > 0) {
        if (strings) {
            maxMemory = __max(maxMemory, initMemory);  /* initMemory <= n */
            strings[index] = malloc(sizeof(TCHAR) * (5 + 10 + 1));  /* Allow up to 10 digits. */
            if (!strings[index]) {
                outOfMemory(TEXT("WBJCAI"), 12);
                return -1;
            }
            _sntprintf(strings[index], 5 + 10 + 1, TEXT("-Xmx%dm"), maxMemory);
        }
        index++;
    }

    /* Store the Wrapper key */
    if (strings) {
        strings[index] = malloc(sizeof(TCHAR) * (16 + _tcslen(wrapperData->key) + 1));
        if (!strings[index]) {
            outOfMemory(TEXT("WBJCAI"), 24);
            return -1;
        }
        _sntprintf(strings[index], 14 + _tcslen(wrapperData->key) + 1, TEXT("-Dwrapper.key=%s"), wrapperData->key);
    }
    index++;
    
    /* Store the backend connection information. */
    if (wrapperData->backendTypeBit == WRAPPER_BACKEND_TYPE_PIPE) {
        if (strings) {
            strings[index] = malloc(sizeof(TCHAR) * (22 + 1));
            if (!strings[index]) {
                outOfMemory(TEXT("WBJCAI"), 25);
                return -1;
            }
            _sntprintf(strings[index], 22 + 1, TEXT("-Dwrapper.backend=pipe"));
        }
        index++;
    } else {

        /* default is socket ipv4, so we have to specify ipv6 if it's the case */
        if (wrapperData->backendTypeBit == WRAPPER_BACKEND_TYPE_SOCKET_V6) {
            if (strings) {
                strings[index] = malloc(sizeof(TCHAR) * (29 + 1));
                if (!strings[index]) {
                    outOfMemory(TEXT("WBJCAI"), 261);
                    return -1;
                }
                _sntprintf(strings[index], 29 + 1, TEXT("-Dwrapper.backend=socket_ipv6"));
            }
            index++;

            /* specify to the JVM to change the preference and use IPv6 addresses over IPv4 ones where possible. */
            if (strings) {
                strings[index] = malloc(sizeof(TCHAR) * (35 + 1));
                if (!strings[index]) {
                    outOfMemory(TEXT("WBJCAI"), 262);
                    return -1;
                }
                _sntprintf(strings[index], 35 + 1, TEXT("-Djava.net.preferIPv6Addresses=TRUE"));
            }
            index++;
        }

        /* Store the Wrapper server port */
        if (strings) {
            strings[index] = malloc(sizeof(TCHAR) * (15 + 5 + 1));  /* Port up to 5 characters */
            if (!strings[index]) {
                outOfMemory(TEXT("WBJCAI"), 26);
                return -1;
            }
            _sntprintf(strings[index], 15 + 5 + 1, TEXT("-Dwrapper.port=%d"), (int)wrapperData->actualPort);
        }
        index++;
    }

    /* Store the Wrapper jvm min and max ports. */
    if ((wrapperData->backendTypeBit == WRAPPER_BACKEND_TYPE_SOCKET_V4) || (wrapperData->backendTypeBit == WRAPPER_BACKEND_TYPE_SOCKET_V6)) {
        if (wrapperData->portAddress != NULL) {
            if (strings) {
                strings[index] = malloc(sizeof(TCHAR) * (_tcslen(TEXT("-Dwrapper.port.address=")) + _tcslen(wrapperData->portAddress) + 1));
                if (!strings[index]) {
                    outOfMemory(TEXT("WBJCAI"), 27);
                    return -1;
                }
                _sntprintf(strings[index], _tcslen(TEXT("-Dwrapper.port.address=")) + _tcslen(wrapperData->portAddress) + 1, TEXT("-Dwrapper.port.address=%s"), wrapperData->portAddress);
            }
            index++;
        }
        
        if (wrapperData->jvmPort >= 0) {
            if (strings) {
                strings[index] = malloc(sizeof(TCHAR) * (19 + 5 + 1));  /* Port up to 5 characters */
                if (!strings[index]) {
                    outOfMemory(TEXT("WBJCAI"), 28);
                    return -1;
                }
                _sntprintf(strings[index], 19 + 5 + 1, TEXT("-Dwrapper.jvm.port=%d"), wrapperData->jvmPort);
            }
            index++;
        }
        
        if (strings) {
            strings[index] = malloc(sizeof(TCHAR) * (23 + 5 + 1));  /* Port up to 5 characters */
            if (!strings[index]) {
                outOfMemory(TEXT("WBJCAI"), 29);
                return -1;
            }
            _sntprintf(strings[index], 23 + 5 + 1, TEXT("-Dwrapper.jvm.port.min=%d"), wrapperData->jvmPortMin);
        }
        index++;
        
        if (strings) {
            strings[index] = malloc(sizeof(TCHAR) * (23 + 5 + 1));  /* Port up to 5 characters */
            if (!strings[index]) {
                outOfMemory(TEXT("WBJCAI"), 30);
                return -1;
            }
            _sntprintf(strings[index], 23 + 5 + 1, TEXT("-Dwrapper.jvm.port.max=%d"), wrapperData->jvmPortMax);
        }
        index++;
 #ifndef WIN32
    } else {
        if (protocolPipeInFd[1] != -1) {
            if (strings) {
                strings[index] = malloc(sizeof(TCHAR) * (_tcslen(TEXT("-Dwrapper.pipe.in=")) + 7 + 1));
                if (!strings[index]) {
                    outOfMemory(TEXT("WBJCAI"), 31);
                    return -1;
                }
                _sntprintf(strings[index], _tcslen(TEXT("-Dwrapper.pipe.in=")) + 7 + 1, TEXT("-Dwrapper.pipe.in=%d"), protocolPipeInFd[1]);
            }
            index++;
        }
        if (protocolPipeOuFd[0] != -1) {
            if (strings) {
                strings[index] = malloc(sizeof(TCHAR) * (_tcslen(TEXT("-Dwrapper.pipe.out=")) + 7 + 1));
                if (!strings[index]) {
                    outOfMemory(TEXT("WBJCAI"), 32);
                    return -1;
                }
                _sntprintf(strings[index], _tcslen(TEXT("-Dwrapper.pipe.out=")) + 7 + 1, TEXT("-Dwrapper.pipe.out=%d"), protocolPipeOuFd[0]);
            }
            index++;
        }
 #endif
    }

    /* Store the Wrapper debug flag */
    if (wrapperData->isDebugging) {
        if (strings) {
            strings[index] = malloc(sizeof(TCHAR) * (22 + 1));
            if (!strings[index]) {
                outOfMemory(TEXT("WBJCAI"), 33);
                return -1;
            }
            _sntprintf(strings[index], 22 + 1, TEXT("-Dwrapper.debug=TRUE"));
        }
        index++;
    }

    /* Store the Wrapper disable console input flag. */
    if (wrapperData->disableConsoleInput) {
        if (strings) {
            strings[index] = malloc(sizeof(TCHAR) * (38 + 1));
            if (!strings[index]) {
                outOfMemory(TEXT("WBJCAI"), 34);
                return -1;
            }
            _sntprintf(strings[index], 38 + 1, TEXT("-Dwrapper.disable_console_input=TRUE"));
        }
        index++;
    }

    /* Store the Wrapper listener force stop flag. */
    if (getBooleanProperty(properties, TEXT("wrapper.listener.force_stop"), FALSE)) {
        if (strings) {
            strings[index] = malloc(sizeof(TCHAR) * (38 + 1));
            if (!strings[index]) {
                outOfMemory(TEXT("WBJCAI"), 35);
                return -1;
            }
            _sntprintf(strings[index], 38 + 1, TEXT("-Dwrapper.listener.force_stop=TRUE"));
        }
        index++;
    }

    /* Store the Wrapper PID */
    if (strings) {
        strings[index] = malloc(sizeof(TCHAR) * (24 + 1)); /* Pid up to 10 characters */
        if (!strings[index]) {
            outOfMemory(TEXT("WBJCAI"), 36);
            return -1;
        }
#if defined(SOLARIS) && (!defined(_LP64))
        _sntprintf(strings[index], 24 + 1, TEXT("-Dwrapper.pid=%ld"), wrapperData->wrapperPID);
#else
        _sntprintf(strings[index], 24 + 1, TEXT("-Dwrapper.pid=%d"), wrapperData->wrapperPID);
#endif
    }
    index++;

    /* Store a flag telling the JVM to use the system clock. */
    if (wrapperData->useSystemTime) {
        if (strings) {
            strings[index] = malloc(sizeof(TCHAR) * (32 + 1));
            if (!strings[index]) {
                outOfMemory(TEXT("WBJCAI"), 37);
                return -1;
            }
            _sntprintf(strings[index], 32 + 1, TEXT("-Dwrapper.use_system_time=TRUE"));
        }
        index++;
    } else {
        /* Only pass the timer fast and slow thresholds to the JVM if they are not default.
         *  These are only used if the system time is not being used. */
        if (wrapperData->timerFastThreshold != WRAPPER_TIMER_FAST_THRESHOLD) {
            if (strings) {
                strings[index] = malloc(sizeof(TCHAR) * (43 + 1)); /* Allow for 10 digits */
                if (!strings[index]) {
                    outOfMemory(TEXT("WBJCAI"), 38);
                    return -1;
                }
                _sntprintf(strings[index], 43 + 1, TEXT("-Dwrapper.timer_fast_threshold=%d"), wrapperData->timerFastThreshold * WRAPPER_TICK_MS / 1000);
            }
            index++;
        }
        if (wrapperData->timerSlowThreshold != WRAPPER_TIMER_SLOW_THRESHOLD) {
            if (strings) {
                strings[index] = malloc(sizeof(TCHAR) * (43 + 1)); /* Allow for 10 digits */
                if (!strings[index]) {
                    outOfMemory(TEXT("WBJCAI"), 39);
                    return -1;
                }
                _sntprintf(strings[index], 43 + 1, TEXT("-Dwrapper.timer_slow_threshold=%d"), wrapperData->timerSlowThreshold * WRAPPER_TICK_MS / 1000);
            }
            index++;
        }
    }

    /* Always write the version of the wrapper binary as a property.  The
     *  WrapperManager class uses it to verify that the version matches. */
    if (strings) {
        strings[index] = malloc(sizeof(TCHAR) * (20 + _tcslen(wrapperVersion) + 1));
        if (!strings[index]) {
            outOfMemory(TEXT("WBJCAI"), 40);
            return -1;
        }
        _sntprintf(strings[index], 18 + _tcslen(wrapperVersion) + 1, TEXT("-Dwrapper.version=%s"), wrapperVersion);
    }
    index++;

    /* Store the base name of the native library. */
    if (strings) {
        strings[index] = malloc(sizeof(TCHAR) * (27 + _tcslen(wrapperData->nativeLibrary) + 1));
        if (!strings[index]) {
            outOfMemory(TEXT("WBJCAI"), 41);
            return -1;
        }
        _sntprintf(strings[index], 25 + _tcslen(wrapperData->nativeLibrary) + 1, TEXT("-Dwrapper.native_library=%s"), wrapperData->nativeLibrary);
    }
    index++;

    /* Store the arch name of the wrapper. */
    if (strings) {
        strings[index] = malloc(sizeof(TCHAR) * (17 + _tcslen(wrapperArch) + 1));
        if (!strings[index]) {
            outOfMemory(TEXT("WBJCAI"), 42);
            return -1;
        }
        _sntprintf(strings[index], 15 + _tcslen(wrapperArch) + 1, TEXT("-Dwrapper.arch=%s"), wrapperArch);
    }
    index++;

    /* Store the ignore signals flag if configured to do so */
    if (wrapperData->ignoreSignals & WRAPPER_IGNORE_SIGNALS_JAVA) {
        if (strings) {
            strings[index] = malloc(sizeof(TCHAR) * (31 + 1));
            if (!strings[index]) {
                outOfMemory(TEXT("WBJCAI"), 43);
                return -1;
            }
            _sntprintf(strings[index], 31 + 1, TEXT("-Dwrapper.ignore_signals=TRUE"));
        }
        index++;
    }

    /* If this is being run as a service, add a service flag. */
    if (!wrapperData->isConsole) {
        if (strings) {
            strings[index] = malloc(sizeof(TCHAR) * (24 + 1));
            if (!strings[index]) {
                outOfMemory(TEXT("WBJCAI"), 44);
                return -1;
            }
            _sntprintf(strings[index], 24 + 1, TEXT("-Dwrapper.service=TRUE"));
        }
        index++;
    }

    /* Store the Disable Tests flag */
    if (wrapperData->isTestsDisabled) {
        if (strings) {
            strings[index] = malloc(sizeof(TCHAR) * (30 + 1));
            if (!strings[index]) {
                outOfMemory(TEXT("WBJCAI"), 45);
                return -1;
            }
            _sntprintf(strings[index], 30 + 1, TEXT("-Dwrapper.disable_tests=TRUE"));
        }
        index++;
    }

    /* Store the Disable Shutdown Hook flag */
    if (wrapperData->isShutdownHookDisabled) {
        if (strings) {
            strings[index] = malloc(sizeof(TCHAR) * (38 + 1));
            if (!strings[index]) {
                outOfMemory(TEXT("WBJCAI"), 46);
                return -1;
            }
            _sntprintf(strings[index], 38 + 1, TEXT("-Dwrapper.disable_shutdown_hook=TRUE"));
        }
        index++;
    }
    
    /* Store the CPU Timeout value */
    if (strings) {
        /* Just to be safe, allow 20 characters for the timeout value */
        strings[index] = malloc(sizeof(TCHAR) * (24 + 20 + 1));
        if (!strings[index]) {
            outOfMemory(TEXT("WBJCAI"), 47);
            return -1;
        }
        _sntprintf(strings[index], 24 + 20 + 1, TEXT("-Dwrapper.cpu.timeout=%d"), wrapperData->cpuTimeout);
    }
    index++;

    if ((prop = getStringProperty(properties, TEXT("wrapper.java.outfile"), NULL))) {
        if (strings) {
            strings[index] = malloc(sizeof(TCHAR) * (25 + _tcslen(prop) + 1));
            if (!strings[index]) {
                outOfMemory(TEXT("WBJCAI"), 49);
                return -1;
            }
            _sntprintf(strings[index], 25 + _tcslen(prop) + 1, TEXT("-Dwrapper.java.outfile=%s"), prop);
        }
        index++;
    }

    if ((prop = getStringProperty(properties, TEXT("wrapper.java.errfile"), NULL))) {
        if (strings) {
            strings[index] = malloc(sizeof(TCHAR) * (25 + _tcslen(prop) + 1));
            if (!strings[index]) {
                outOfMemory(TEXT("WBJCAI"), 50);
                return -1;
            }
            _sntprintf(strings[index], 25 + _tcslen(prop) + 1, TEXT("-Dwrapper.java.errfile=%s"), prop);
        }
        index++;
    }

    /* Store the Wrapper JVM ID.  (Get here before incremented) */
    if (strings) {
        strings[index] = malloc(sizeof(TCHAR) * (16 + 5 + 1));  /* jvmid up to 5 characters */
        if (!strings[index]) {
            outOfMemory(TEXT("WBJCAI"), 49);
            return -1;
        }
        _sntprintf(strings[index], 16 + 5 + 1, TEXT("-Dwrapper.jvmid=%d"), (wrapperData->jvmRestarts + 1));
    }
    index++;


    /* If this JVM will be detached after startup, it needs to know that. */
    if (wrapperData->detachStarted) {
        if (strings) {
            strings[index] = malloc(sizeof(TCHAR) * (30 + 1));
            if (!strings[index]) {
                outOfMemory(TEXT("WBJCAI"), 53);
                return -1;
            }
            _sntprintf(strings[index], 30 + 1, TEXT("-Dwrapper.detachStarted=TRUE"));
        }
        index++;
    }

    /* Library Path */
    if ((index = wrapperBuildJavaCommandArrayLibraryPath(strings, index)) < 0) {
        return -1;
    }

    /* Classpath */
    if (!wrapperData->environmentClasspath) {
        if ((index = wrapperBuildJavaCommandArrayClasspath(strings, index, classpath)) < 0) {
            return -1;
        }
    }

    /* Module Path */
    if ((index = wrapperBuildJavaCommandArrayModulepath(strings, index, wrapperData->modulePath)) < 0) {
        return -1;
    }

    /* Upgrade Module Path */
    if ((index = wrapperBuildJavaCommandArrayUpgradeModulepath(strings, index, wrapperData->upgradeModulePath)) < 0) {
        return -1;
    }

    /* Modules List */
    if ((index = wrapperBuildJavaCommandArrayModulelist(strings, index, wrapperData->moduleList)) < 0) {
        return -1;
    }

    /* Enable Native Access List (Java 24+) */
    if (wrapperData->nativeAccessModuleList || wrapperData->addWrapperToNativeAccessModuleList) {
        if ((index = wrapperBuildJavaCommandArrayNativeAccesslist(strings, index, wrapperData->nativeAccessModuleList, wrapperData->addWrapperToNativeAccessModuleList)) < 0) {
            return -1;
        }
    }

    /* Add-opens
     *  Note: Multiple '--add-opens' may be listed in the command line. It
     *        doesn't hurt to add ours even if '<package>/<mainclass>=...'
     *        also exists in the java additionals. */
    if (wrapperData->jvmAddOpens) {
        if (!wrapperData->mainModule || !wrapperData->mainUsrPkg) {
            /* This should not happen if the parsing of the bootstrap step was correct.. but keep this condition as a sanity check. */
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Failed to build the --add-opens clause of the command line."));
            return -1;
        }
        if (strings) {
            strings[index] = malloc(sizeof(TCHAR) * (11 + 1));
            if (!strings[index]) {
                outOfMemory(TEXT("WBJCAI"), 14);
                return -1;
            }
            _sntprintf(strings[index], 11 + 1, TEXT("--add-opens"));
        }
        index++;
        if (strings) {
            moduleLen = _tcslen(wrapperData->mainModule);
            packageLen = _tcslen(wrapperData->mainUsrPkg);
            strings[index] = malloc(sizeof(TCHAR) * (moduleLen + 1 + packageLen + 27 + 1));
            if (!strings[index]) {
                outOfMemory(TEXT("WBJCAI"), 15);
                return -1;
            }
            _sntprintf(strings[index], moduleLen + 1 + packageLen + 27 + 1, TEXT("%s/%s=org.tanukisoftware.wrapper"), wrapperData->mainModule, wrapperData->mainUsrPkg);
        }
        index++;
    }

    /* Store the main class */
    prop = getStringProperty(properties, TEXT("wrapper.java.mainclass"), TEXT("Main"));
    if (_tcscmp(prop, TEXT("org.tanukisoftware.wrapper.test.Main")) == 0) {
        wrapperData->thisIsTestWrapper = TRUE;
    } else {
        wrapperData->thisIsTestWrapper = FALSE;
    }
    if (strings) {
        strings[index] = malloc(sizeof(TCHAR) * (_tcslen(prop) + 1));
        if (!strings[index]) {
            outOfMemory(TEXT("WBJCAI"), 54);
            return -1;
        }
        _sntprintf(strings[index], _tcslen(prop) + 1, TEXT("%s"), prop);
    }
    index++;

    /* Store any application parameters */
    if (!wrapperData->useBackendParameters) {
        if ((index = wrapperBuildAppParameterArrayInner(strings, index, wrapperData->thisIsTestWrapper, FALSE)) < 0) {
            return -1;
        }
    }

    return index;
}

/**
 * command is a pointer to a pointer of an array of character strings.
 * length is the number of strings in the above array.
 *
 * @return TRUE if there were any problems.
 */
int wrapperBuildJavaCommandArray(TCHAR ***stringsPtr, int *length, const TCHAR *classpath) {
    int reqLen;

    /* Reset the flag stating that the JVM is a debug JVM. */
    wrapperData->debugJVM = FALSE;
    wrapperData->debugJVMTimeoutNotified = FALSE;

    /* Find out how long the array needs to be first. */
    reqLen = wrapperBuildJavaCommandArrayInner(NULL, classpath);
    if (reqLen < 0) {
        return TRUE;
    }
    *length = reqLen;

    /* Allocate the correct amount of memory */
    *stringsPtr = malloc((*length) * sizeof **stringsPtr);
    if (!(*stringsPtr)) {
        outOfMemory(TEXT("WBJCA"), 1);
        return TRUE;
    }
    memset(*stringsPtr, 0, (*length) * sizeof **stringsPtr);

    /* Now actually fill in the strings */
    reqLen = wrapperBuildJavaCommandArrayInner(*stringsPtr, classpath);
    if (reqLen < 0) {
        return TRUE;
    }

    if (wrapperData->debugJVM) {
        if ((wrapperData->startupTimeout > 0) || (wrapperData->pingTimeout > 0) ||
            (wrapperData->shutdownTimeout > 0) || (wrapperData->jvmExitTimeout > 0)) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN,
                TEXT("---------------------------------------------------------------------") );
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN,
                TEXT(
                     "The JVM is being launched with a debugger enabled and could possibly\nbe suspended.  To avoid unwanted shutdowns, timeouts will be\ndisabled, removing the ability to detect and restart frozen JVMs."));
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN,
                TEXT("---------------------------------------------------------------------") );
        }
    }

    return FALSE;
}

static int wrapperBuildJavaBootstrapCommandArrayInner(TCHAR **strings, int id, const TCHAR* entryPoint) {
    int index = 0;
    size_t len;
#ifdef WIN32
    TCHAR encodingBuff[ENCODING_BUFFER_SIZE];
#endif

    /* Command */
    if (!wrapperData->javaQryCmd) {
        return -1;
    }
    if (strings) {
        len = _tcslen(wrapperData->javaQryCmd);
        strings[index] = malloc(sizeof(TCHAR) * (len + 1));
        if (!strings[index]) {
            outOfMemory(TEXT("WBBCAI"), 1);
            return -1;
        }
        _sntprintf(strings[index], len + 1, TEXT("%s"), wrapperData->javaQryCmd);
    }
    index++;

    /* Additional java parameters */
    if ((index = wrapperBuildJavaCommandArrayJavaAdditional(strings, FALSE, JVM_TARGET_BOOTSTRAP, TRUE, index)) < 0) {
        return -1;
    }

#ifdef WIN32
    /* This command will be launched using the locale encoding. On Windows, the JVM encoding is not always inherited from
     *  the parent process, so we need to pass it with file.encoding. This is needed because some elements of the bootstrap
     *  output (name of mainclass, of the jar file, etc.) may use non-ASCII characters (although this should remain rare). */
    if (getBooleanProperty(properties, TEXT("wrapper.java.query.pass_encoding"), getBooleanProperty(properties, TEXT("wrapper.java.pass_encoding"), TRUE))) {
        if (!getJvmIoEncodingFromCodePage(wrapperData->jvm_stdout_codepage, wrapperData->javaVersion->major, encodingBuff)) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Failed to set encoding used to read Java bootstrap output."));
            return -1;
        }
        if (strings) {
            len = 16 + _tcslen(encodingBuff);
            strings[index] = malloc(sizeof(TCHAR) * (len + 1));
            if (!strings[index]) {
                outOfMemory(TEXT("WBBCAI"), 2);
                return -1;
            }
            _sntprintf(strings[index], len + 1, TEXT("-Dfile.encoding=%s"), encodingBuff);
        }
        index++;
    }
#endif

    /* Classpath */
    if (!wrapperData->environmentClasspath) {
        if ((index = wrapperBuildJavaCommandArrayClasspath(strings, index, wrapperData->classpath)) < 0) {
            return -1;
        }
    }

    /* Module Path */
    if ((index = wrapperBuildJavaCommandArrayModulepath(strings, index, wrapperData->modulePath)) < 0) {
        return -1;
    }

    /* Upgrade Module Path */
    if ((index = wrapperBuildJavaCommandArrayUpgradeModulepath(strings, index, wrapperData->upgradeModulePath)) < 0) {
        return -1;
    }

    /* Modules List */
    if ((index = wrapperBuildJavaCommandArrayModulelist(strings, index, wrapperData->moduleList)) < 0) {
        return -1;
    }

    /* Main Class */
    if (strings) {
        len = 53;
        strings[index] = malloc(sizeof(TCHAR) * (len + 1));
        if (!strings[index]) {
            outOfMemory(TEXT("WBBCAI"), 3);
            return -1;
        }
        _sntprintf(strings[index], len + 1, TEXT("org.tanukisoftware.wrapper.bootstrap.WrapperBootstrap"));
    }
    index++;

    /* Parameters */
    if (strings) {
        len = 1;
        strings[index] = malloc(sizeof(TCHAR) * (len + 1));
        if (!strings[index]) {
            outOfMemory(TEXT("WBBCAI"), 4);
            return -1;
        }
        _sntprintf(strings[index], len + 1, TEXT("%d"), id);
    }
    index++;

    if (strings) {
        len = _tcslen(entryPoint);
        strings[index] = malloc(sizeof(TCHAR) * (len + 1));
        if (!strings[index]) {
            outOfMemory(TEXT("WBBCAI"), 5);
            return -1;
        }
        _sntprintf(strings[index], len + 1, TEXT("%s"), entryPoint);
    }
    index++;

    if (strings) {
        len = 1;
        strings[index] = malloc(sizeof(TCHAR) * (len + 1));
        if (!strings[index]) {
            outOfMemory(TEXT("WBBCAI"), 6);
            return -1;
        }
        _sntprintf(strings[index], len + 1, TEXT("%d"), wrapperData->isDebugging ? 1 : 0);
    }
    index++;

    return index;
}

int wrapperBuildJavaBootstrapCommandArray(TCHAR ***stringsPtr, int *length, int id, const TCHAR* entryPoint) {
    int reqLen;

    /* Find out how long the array needs to be first. */
    reqLen = wrapperBuildJavaBootstrapCommandArrayInner(NULL, id, entryPoint);
    if (reqLen < 0) {
        return TRUE;
    }
    *length = reqLen;

    /* Allocate the correct amount of memory */
    *stringsPtr = malloc((*length) * sizeof **stringsPtr);
    if (!(*stringsPtr)) {
        outOfMemory(TEXT("WBBCA"), 1);
        return TRUE;
    }
    memset(*stringsPtr, 0, (*length) * sizeof **stringsPtr);

    /* Now actually fill in the strings */
    reqLen = wrapperBuildJavaBootstrapCommandArrayInner(*stringsPtr, id, entryPoint);
    if (reqLen < 0) {
        return TRUE;
    }
    return FALSE;
}

void wrapperFreeStringArray(TCHAR **strings, int length) {
    int i;

    if (strings != NULL) {
        /* Loop over and free each of the strings in the array */
        for (i = 0; i < length; i++) {
            if (strings[i] != NULL) {
                free(strings[i]);
                strings[i] = NULL;
            }
        }
        free(strings);
        strings = NULL;
    }
}

void wrapperSecureFreeStringArray(TCHAR **strings, int length) {
    int i;

    if (strings != NULL) {
        /* Loop over and free each of the strings in the array */
        for (i = 0; i < length; i++) {
            if (strings[i] != NULL) {
                wrapperSecureFreeStrW(strings[i]);
                strings[i] = NULL;
            }
        }
        free(strings);
        strings = NULL;
    }
}

#ifdef WIN32
static void wrapperPrintExitCodeDescription(int exitCode) {
    const TCHAR* message = NULL;
    TCHAR* ptr = NULL;
    int firstLine = TRUE;
    int marginLen;
    HMODULE handle;
    
    handle = LoadLibrary(TEXT("ntdll.dll"));
    if (handle) {
        /* Try get a message description from ntdll. It contains most common system errors thrown when a program crashes. */
        SetLastError(0);
        message = getErrorText(exitCode, handle);
        if ((GetLastError() == ERROR_MR_MID_NOT_FOUND) ||       /* A message description does not exist in the system for the exit code. */
            (message[0] == TEXT('\0')) ||                       /* Empty message. */
            (_tcschr(message, TEXT('%')) != NULL)) {            /* Message contains parameters which we can't print... */
            message = NULL;
        }
        FreeLibrary(handle);
    }
    
    if (!message) {
        /* Try get a constant from our internal function. */
        message = getExceptionName(exitCode, TRUE);
    }
    
    if (message) {
        /* The message may contain several lines. Print line by line. */
        do {
            ptr = _tcschr(message, TEXT('\n'));
            if (ptr) {
                *ptr = 0;
                ptr++;
            }
            if (firstLine) {
                /* first line: print the error code */
                firstLine = FALSE;
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("  %d: %s"), exitCode, message);
            } else {
                /* line > 1: print a margin to align with the first line. */
                marginLen = (int)floor (log10 ((double)abs (exitCode))) + 1;
                if (exitCode < 0) {
                    marginLen++;
                }
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("  %*s  %s"), marginLen, TEXT(""), message);
            }
            message = ptr;
        } while (message);
    }
}
#endif

/**
 * Called when the Wrapper detects that the JVM process has exited.
 *  Contains code common to all platforms.
 */
void wrapperJVMProcessExited(TICKS nowTicks, int exitCode) {
    int setState = TRUE;
    int logLevel = LEVEL_DEBUG;
#ifdef WIN32
    int printCrashStatusDescription = FALSE;
#endif

    if (!wrapperData->useJavaIOThread) {
        /* Make sure there is no JVM output left in the pipe.  This must be done before
         *  resetting the java PID for the 'J' format of these messages to be correct. */
        while (wrapperReadChildOutput(250)) {};
    }

    /* Reset the Java PID.
     *  Also do it in wrapperJVMDownCleanup(), whichever is called first. */
    resetJavaPid();

    if ((wrapperData->jState == WRAPPER_JSTATE_LAUNCHED) ||
        (wrapperData->jState == WRAPPER_JSTATE_STARTING) ||
        (wrapperData->jState == WRAPPER_JSTATE_STARTED) ||
        (wrapperData->jState == WRAPPER_JSTATE_STOP) ||
        (wrapperData->jState == WRAPPER_JSTATE_STOPPING)) {
        /* The JVM crashed (it terminated before the STOPPED state was confirmed). Always make sure to print its status code. */
        logLevel = LEVEL_STATUS;

#ifdef WIN32
        /* If the Java application crashed and the JVM couldn't handle the exception, an exit code in the 0xC0... range will be returned.
         *  In such cases, the JVM most likely did not have a chance to print a report in its exception handler. */
        if ((NTSTATUS)exitCode > (unsigned long)0xc0000001) {
            /* NOTE: There is no upper limit for the range as more errors may be added in the future. There is also the
             *        possibility for the exit code to simply be user defined (such as negative values which will be wrapped).
             *        At the very least the print function below will only print messages that exist as standard errors. */
            printCrashStatusDescription = TRUE;
        }
#endif
    }

    /* The error codes are printed as signed integers and may be wrapped. This is done because the Wrapper exit
     *  code, which is a signed integer, may be set to the JVM exit code. Backward compatibility needs to be kept. */
    if (exitCode == 0) {
        /* The JVM exit code was 0, so leave any current exit code as is. */
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG,
            TEXT("JVM process exited with a code of %d, leaving the Wrapper exit code set to %d."),
            exitCode, wrapperData->exitCode);

    } else if (wrapperData->exitCode == 0) {
        /* Update the Wrapper exitCode. */
        wrapperData->exitCode = exitCode;
        log_printf(WRAPPER_SOURCE_WRAPPER, logLevel,
            TEXT("JVM process exited with a code of %d, setting the Wrapper exit code to %d."),
            exitCode, wrapperData->exitCode);

    } else {
        /* The Wrapper exit code was already non-zero, so leave it as is. */
        log_printf(WRAPPER_SOURCE_WRAPPER, logLevel,
            TEXT("JVM process exited with a code of %d, however the Wrapper exit code was already %d."),
            exitCode, wrapperData->exitCode);
    }

#ifdef WIN32
    if (printCrashStatusDescription) {
        wrapperPrintExitCodeDescription(exitCode);
    }
#endif

    switch(wrapperData->jState) {
    case WRAPPER_JSTATE_DOWN_CLEAN:
    case WRAPPER_JSTATE_DOWN_CHECK:
    case WRAPPER_JSTATE_DOWN_FLUSH_STDIN:
    case WRAPPER_JSTATE_DOWN_FLUSH:
        /* Shouldn't be called in this state.  But just in case. */
        if (wrapperData->isDebugging) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG,
                TEXT("JVM already down."));
        }
        setState = FALSE;
        break;

    case WRAPPER_JSTATE_LAUNCH_DELAY:
        /* We got a message that the JVM process died when we already thought is was down.
         *  Most likely this was caused by a SIGCHLD signal.  We are already in the expected
         *  state so go ahead and ignore it.  Do NOT go back to DOWN or the restart flag
         *  and all restart counts will have be lost */
        if (wrapperData->isDebugging) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG,
                TEXT("Received a message that the JVM is down when in the LAUNCH(DELAY) state."));
        }
        setState = FALSE;
        break;

    case WRAPPER_JSTATE_RESTART:
        /* We got a message that the JVM process died when we already thought is was down.
         *  Most likely this was caused by a SIGCHLD signal.  We are already in the expected
         *  state so go ahead and ignore it.  Do NOT go back to DOWN or the restart flag
         *  and all restart counts will have be lost */
        if (wrapperData->isDebugging) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG,
                TEXT("Received a message that the JVM is down when in the RESTART state."));
        }
        setState = FALSE;
        break;

    case WRAPPER_JSTATE_LAUNCH:
        /* We got a message that the JVM process died when we already thought is was down.
         *  Most likely this was caused by a SIGCHLD signal.  We are already in the expected
         *  state so go ahead and ignore it.  Do NOT go back to DOWN or the restart flag
         *  and all restart counts will have be lost.
         * This can happen if the Java process dies Immediately after it is launched.  It
         *  is very rare if Java is launched, but can happen if the configuration is set to
         *  launch something else. */
        if (wrapperData->isDebugging) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG,
                TEXT("Received a message that the JVM is down when in the LAUNCH state."));
        }
        setState = FALSE;
        break;

    case WRAPPER_JSTATE_LAUNCHING:
        wrapperData->restartRequested = WRAPPER_RESTART_REQUESTED_AUTOMATIC;
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR,
            TEXT("JVM exited while loading the application."));
        break;

    case WRAPPER_JSTATE_LAUNCHED:
        /* Shouldn't be called in this state, but just in case. */
        wrapperData->restartRequested = WRAPPER_RESTART_REQUESTED_AUTOMATIC;
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR,
           TEXT("JVM exited before starting the application."));
        break;

    case WRAPPER_JSTATE_STARTING:
        wrapperData->restartRequested = WRAPPER_RESTART_REQUESTED_AUTOMATIC;
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR,
            TEXT("JVM exited while starting the application."));
        break;

    case WRAPPER_JSTATE_STARTED:
        wrapperData->restartRequested = WRAPPER_RESTART_REQUESTED_AUTOMATIC;
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR,
            TEXT("JVM exited unexpectedly."));
        break;

    case WRAPPER_JSTATE_STOP:
    case WRAPPER_JSTATE_STOPPING:
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN,
            TEXT("JVM exited unexpectedly while stopping the application."));
        break;

    case WRAPPER_JSTATE_STOPPED:
        if (wrapperData->stoppedPacketReceived) {
            /* This is the most common case. */
            if (wrapperData->isDebugging) {
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG,
                    TEXT("JVM exited normally."));
            }
        } else {
            /* This can happen when the backend connection closed unexpectedly.
             *  We weren't able to receive the STOPPED packed but we went to the
             *  WRAPPER_JSTATE_STOPPED state to wait for the process to exit. */
            if (wrapperData->isDebugging) {
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG,
                    TEXT("JVM exited on its own."));
            }
        }
        break;

    case WRAPPER_JSTATE_KILLING:
    case WRAPPER_JSTATE_KILL:
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_INFO,
            TEXT("JVM exited on its own while waiting to kill the application."));
        break;

    case WRAPPER_JSTATE_KILLED:
        if (wrapperData->jvmTerminatedBeforeKill) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS,
                TEXT("JVM exited on its own before being requested to terminate."));
        } else {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS,
                TEXT("JVM exited after being requested to terminate."));
        }
        break;

    default:
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN,
            TEXT("Unexpected jState=%d in wrapperJVMProcessExited."), wrapperData->jState);
        break;
    }

    wrapperJVMDownCleanup(setState);
}

void wrapperBuildKey() {
    int i;
    size_t kcNum;
    size_t num;
    static int seeded = FALSE;

    /* Seed the randomizer */
    if (!seeded) {
        srand((unsigned)time(NULL));
        seeded = TRUE;
    }

    /* Start by generating a key */
    num = _tcslen(keyChars);

    for (i = 0; i < 16; i++) {
        /* The way rand works, this will sometimes equal num, which is too big.
         *  This is rare so just round those cases down. */

        /* Some platforms use very large RAND_MAX values that cause overflow problems in our math */
        if (RAND_MAX > 0x10000) {
            kcNum = (size_t)((rand() >> 8) * num / (RAND_MAX >> 8));
        } else {
            kcNum = (size_t)(rand() * num / RAND_MAX);
        }

        if (kcNum >= num) {
            kcNum = num - 1;
        }

        wrapperData->key[i] = keyChars[kcNum];
    }
    wrapperData->key[16] = TEXT('\0');

    /*
    printf("  Key=%s Len=%lu\n", wrapperData->key, _tcslen(wrapperData->key));
    */
}

#ifdef WIN32

/* The ABOVE and BELOW normal priority class constants are not defined in MFVC 6.0 headers. */
#ifndef ABOVE_NORMAL_PRIORITY_CLASS
#define ABOVE_NORMAL_PRIORITY_CLASS 0x00008000
#endif
#ifndef BELOW_NORMAL_PRIORITY_CLASS
#define BELOW_NORMAL_PRIORITY_CLASS 0x00004000
#endif

/**
 * Return FALSE if successful, TRUE if there were problems.
 */
int wrapperBuildNTServiceInfo() {
    TCHAR *work;
    const TCHAR *priority;
    size_t len;
    size_t valLen;
    size_t workLen;
    int i;
    TCHAR **propertyNames;
    TCHAR **propertyValues;
    long unsigned int *propertyIndices;
    TCHAR *tempAccount;
    PSID pSid1 = NULL;
    PSID pSid2 = NULL;
    int accountType1;
    int accountType2;
    int accountChanged = FALSE;

    if (!wrapperData->configured) {
        /* Load the service load order group */
        updateStringValue(&wrapperData->ntServiceLoadOrderGroup, getStringProperty(properties, TEXT("wrapper.ntservice.load_order_group"), TEXT("")));

        if (getStringProperties(properties, TEXT("wrapper.ntservice.dependency."), TEXT(""), wrapperData->ignoreSequenceGaps, FALSE, &propertyNames, &propertyValues, &propertyIndices)) {
            /* Failed */
            return TRUE;
        }

        /* Build the dependency list.  Decide how large the list needs to be */
        len = 0;
        i = 0;
        while (propertyNames[i]) {
            valLen = _tcslen(propertyValues[i]);
            if (valLen > 0) {
                len += valLen + 1;
            }
            i++;
        }
        /* List must end with a double '\0'.  If the list is not empty then it will end with 3.  But that is fine. */
        len += 2;

        /* Actually build the buffer */
        if (wrapperData->ntServiceDependencies) {
            /** This is a reload, so free up the old data. */
            free(wrapperData->ntServiceDependencies);
            wrapperData->ntServiceDependencies = NULL;
        }
        work = wrapperData->ntServiceDependencies = malloc(sizeof(TCHAR) * len);
        if (!work) {
            outOfMemory(TEXT("WBNTSI"), 1);
            return TRUE;
        }
        workLen = len;

        /* Now actually build up the list. Each value is separated with a '\0'. */
        i = 0;
        while (propertyNames[i]) {
            valLen = _tcslen(propertyValues[i]);
            if (valLen > 0) {
                _tcsncpy(work, propertyValues[i], workLen);
                work += valLen + 1;
                workLen -= valLen + 1;
            }
            i++;
        }
        /* Add two more nulls to the end of the list. */
        work[0] = TEXT('\0');
        work[1] = TEXT('\0');

        /* Memory allocated in work is stored in wrapperData.  The memory should not be released here. */
        work = NULL;

        freeStringProperties(propertyNames, propertyValues, propertyIndices);

        /* Set the service start type */
        if (strcmpIgnoreCase(getStringProperty(properties, TEXT("wrapper.ntservice.starttype"), TEXT("DEMAND_START")), TEXT("AUTO_START")) == 0) {
            wrapperData->ntServiceStartType = SERVICE_AUTO_START;
        } else {
            wrapperData->ntServiceStartType = SERVICE_DEMAND_START;
        }

        /* Set the service priority class */
        priority = getStringProperty(properties, TEXT("wrapper.ntservice.process_priority"), TEXT("NORMAL"));
        if ( (strcmpIgnoreCase(priority, TEXT("LOW")) == 0) || (strcmpIgnoreCase(priority, TEXT("IDLE")) == 0) ) {
            wrapperData->ntServicePriorityClass = IDLE_PRIORITY_CLASS;
        } else if (strcmpIgnoreCase(priority, TEXT("HIGH")) == 0) {
            wrapperData->ntServicePriorityClass = HIGH_PRIORITY_CLASS;
        } else if (strcmpIgnoreCase(priority, TEXT("REALTIME")) == 0) {
            wrapperData->ntServicePriorityClass = REALTIME_PRIORITY_CLASS;
        } else if (strcmpIgnoreCase(priority, TEXT("ABOVE_NORMAL")) == 0) {
            wrapperData->ntServicePriorityClass = ABOVE_NORMAL_PRIORITY_CLASS;
        } else if (strcmpIgnoreCase(priority, TEXT("BELOW_NORMAL")) == 0) {
            wrapperData->ntServicePriorityClass = BELOW_NORMAL_PRIORITY_CLASS;
        } else {
            wrapperData->ntServicePriorityClass = NORMAL_PRIORITY_CLASS;
        }

        /* Account name */
        updateStringValue(&wrapperData->ntServiceAccount, getStringProperty(properties, TEXT("wrapper.ntservice.account"), NULL));
        if (wrapperData->ntServiceAccount && (_tcslen(wrapperData->ntServiceAccount) <= 0)) {
            free(wrapperData->ntServiceAccount);
            wrapperData->ntServiceAccount = NULL;
        }

        /* When running as a service, build the account using the domain and user info previously retrieved from the process token. */
        if (!wrapperData->isConsole) {
            len = _tcslen(wrapperData->domainName) + 1 + _tcslen(wrapperData->userName);
            tempAccount = malloc(sizeof(TCHAR) * (len + 1));
            if (!tempAccount) {
                outOfMemory(TEXT("WBNTSI"), 2);
                return TRUE;
            }
            _sntprintf(tempAccount, len + 1, TEXT("%s\\%s"), wrapperData->domainName, wrapperData->userName);
            getTrusteeSidFromName(tempAccount, &pSid1);
            if (!pSid1) {
                /* tempAccount was built using information of the process token, so normally the SID sould not be NULL. */
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Failed to retrieve SID for account '%s'."), tempAccount);
                free(tempAccount);
                return TRUE;
            }
            if (wrapperData->ntServiceAccount) {
                getTrusteeSidFromName(wrapperData->ntServiceAccount, &pSid2);
                if (!pSid2) {
                    accountType2 = getServiceAccountType(wrapperData->ntServiceAccount, wrapperData->serviceName, TRUE);
                    switch (accountType2) {
                    case NON_EXISTANT_ACCOUNT:
                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN,
                            TEXT("The value of property '%s' (%s) is not a valid user account."),
                            TEXT("wrapper.ntservice.account"), wrapperData->ntServiceAccount); /* note: same message used in wrapperInstall() */
                        break;

                    case VIRTUAL_SERVICE_ACCOUNT:
                    case NETWORK_SERVICE_ACCOUNT:
                    case LOCAL_SERVICE_ACCOUNT:
                    case LOCAL_SYSTEM_ACCOUNT:
                        accountType1 = getServiceAccountType(tempAccount, wrapperData->serviceName, TRUE);
                        if (accountType1 != accountType2) {
                            accountChanged = TRUE;
                        }
                        break;

                    default:
                        break;
                    }
                } else {
                    if (!EqualSid(pSid1, pSid2)) {
                        accountChanged = TRUE;
                    }
                    free(pSid2);
                }
                if (accountChanged) {
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN,
                        TEXT("The %s property has been changed from\n '%s' to '%s' after the service was installed.\n The service is currently running with the %s\n account. Please make sure this is correct."),
                        TEXT("wrapper.ntservice.account"), tempAccount, wrapperData->ntServiceAccount, tempAccount);
                }
            }
            free(pSid1);
            updateStringValue(&wrapperData->ntServiceAccount, tempAccount);
            free(tempAccount);
        }

        if (wrapperData->ntServiceAccount) {
            if (strcmpIgnoreCase(getStringProperty(properties, TEXT("wrapper.ntservice.account.logon_as_service"), TEXT("ALLOW")), TEXT("UNCHANGED")) != 0) {
                wrapperData->ntServiceAddLogonAsService = TRUE;
            }
        }

        /* Account password */
        wrapperData->ntServicePrompt = getBooleanProperty(properties, TEXT("wrapper.ntservice.account.prompt"), FALSE);
        if (wrapperData->ntServicePrompt) {
            wrapperData->ntServicePasswordPrompt = TRUE;
        } else {
            wrapperData->ntServicePasswordPrompt = getBooleanProperty(properties, TEXT("wrapper.ntservice.password.prompt"), FALSE);
        }
        wrapperData->ntServicePasswordPromptMask = getBooleanProperty(properties, TEXT("wrapper.ntservice.password.prompt.mask"), TRUE);
        if (!wrapperData->ntServiceAccount) {
            /* If there is no account name, then the password must not be set. */
            wrapperData->ntServicePassword = NULL;
        } else {
            updateStringValue(&wrapperData->ntServicePassword, getStringProperty(properties, TEXT("wrapper.ntservice.password"), NULL));
            if (wrapperData->ntServicePassword && (_tcslen(wrapperData->ntServicePassword) <= 0)) {
                wrapperSecureFreeStrW(wrapperData->ntServicePassword);
                wrapperData->ntServicePassword = NULL;
            }
        }

        /* Interactive */
        wrapperData->ntServiceInteractive = getBooleanProperty(properties, TEXT("wrapper.ntservice.interactive"), FALSE);
        /* The interactive flag can not be set if an account is also set. */
        if (wrapperData->ntServiceAccount && wrapperData->ntServiceInteractive) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN,
                TEXT("Ignoring the wrapper.ntservice.interactive property because it can not be set when wrapper.ntservice.account is also set."));
            wrapperData->ntServiceInteractive = FALSE;
        }

        /* Display a Console Window. */
        wrapperData->ntAllocConsole = getBooleanProperty(properties, TEXT("wrapper.ntservice.console"), FALSE);
        /* Set the default show wrapper console flag to the value of the alloc console flag. */
        wrapperData->ntShowWrapperConsole = wrapperData->ntAllocConsole;

        /* Hide the JVM Console Window. */
        wrapperData->ntHideJVMConsole = getBooleanProperty(properties, TEXT("wrapper.ntservice.hide_console"), TRUE);

        /* Make sure that a console is always generated to support thread dumps */
        wrapperData->generateConsole = getBooleanProperty(properties, TEXT("wrapper.ntservice.generate_console"), TRUE);
        
        /* Wait hint used when reporting STARTING, RESUMING, PAUSING or STOPPING status to the service controller. */
        wrapperData->ntStartupWaitHint = getIntProperty(properties, TEXT("wrapper.ntservice.startup.waithint"), 30);
        wrapperData->ntShutdownWaitHint = getIntProperty(properties, TEXT("wrapper.ntservice.shutdown.waithint"), 30);
        
        /* Set the range of valid values for the wait hints to [2secs, 1min] */
        if (wrapperData->ntStartupWaitHint > 60) {
            wrapperData->ntStartupWaitHint = 60;
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR,
                TEXT("The value of %s must be at most %d second(s).  Changing to %d."), TEXT("wrapper.ntservice.startup.waithint"), 60, wrapperData->ntStartupWaitHint);
        } else if (wrapperData->ntStartupWaitHint < 2) {
            wrapperData->ntStartupWaitHint = 2;
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR,
                TEXT("The value of %s must be at least %d second(s).  Changing to %d."), TEXT("wrapper.ntservice.startup.waithint"), 2, wrapperData->ntStartupWaitHint);
        }
        if (wrapperData->ntShutdownWaitHint > 60) {
            wrapperData->ntShutdownWaitHint = 60;
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR,
                TEXT("The value of %s must be at most %d second(s).  Changing to %d."), TEXT("wrapper.ntservice.shutdown.waithint"), 60, wrapperData->ntStartupWaitHint);
        } else if (wrapperData->ntShutdownWaitHint < 2) {
            wrapperData->ntShutdownWaitHint = 2;
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR,
                TEXT("The value of %s must be at least %d second(s).  Changing to %d."), TEXT("wrapper.ntservice.shutdown.waithint"), 2, wrapperData->ntStartupWaitHint);
        }
        
#ifdef SUPPORT_PRESHUTDOWN
        /* Accept preshutdown control code? */
        wrapperData->ntPreshutdown = getBooleanProperty(properties, TEXT("wrapper.ntservice.preshutdown"), FALSE);
        if (wrapperData->ntPreshutdown) {
            if (!isVista()) {
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN,
                    TEXT("Property %s is ignored on this system."), TEXT("wrapper.ntservice.preshutdown"));
                removeProperty(properties, TEXT("wrapper.ntservice.preshutdown"));
                wrapperData->ntPreshutdown = FALSE;
            } else if (!strcmpIgnoreCase(wrapperData->argCommand, TEXT("i"))  || !strcmpIgnoreCase(wrapperData->argCommand, TEXT("-install")) ||
                       !strcmpIgnoreCase(wrapperData->argCommand, TEXT("it")) || !strcmpIgnoreCase(wrapperData->argCommand, TEXT("-installstart"))) {
                wrapperData->ntPreshutdownTimeout = getIntProperty(properties, TEXT("wrapper.ntservice.preshutdown.timeout"), 180);
                if (wrapperData->ntPreshutdownTimeout > 3600) {
                    wrapperData->ntPreshutdownTimeout = 3600;
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR,
                        TEXT("The value of %s must be at most %d hour (%d second(s)).  Changing to %d."), TEXT("wrapper.ntservice.preshutdown.timeout"), 1, 3600, wrapperData->ntPreshutdownTimeout);
                } else if (wrapperData->ntPreshutdownTimeout < 1) {
                    wrapperData->ntPreshutdownTimeout = 1;
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR,
                        TEXT("The value of %s must be at least %d second(s).  Changing to %d."), TEXT("wrapper.ntservice.preshutdown.timeout"), 1, wrapperData->ntPreshutdownTimeout);
                }
            }
        }
#else
        if (getBooleanProperty(properties, TEXT("wrapper.ntservice.preshutdown"), FALSE)) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN,
                TEXT("Property %s is not supported on Windows Itanium."), TEXT("wrapper.ntservice.preshutdown"));
            removeProperty(properties, TEXT("wrapper.ntservice.preshutdown"));
        }
#endif
    }

    /* Set the single invocation flag. */
    wrapperData->isSingleInvocation = getBooleanProperty(properties, TEXT("wrapper.single_invocation"), FALSE);

    wrapperData->threadDumpControlCode = getIntProperty(properties, TEXT("wrapper.thread_dump_control_code"), 255);
    if (wrapperData->threadDumpControlCode <= 0) {
        /* Disabled */
    } else if ((wrapperData->threadDumpControlCode < 128) || (wrapperData->threadDumpControlCode > 255)) {
        wrapperData->threadDumpControlCode = 255;
        log_printf(WRAPPER_SOURCE_WRAPPER, properties->logWarningLogLevel,
            TEXT("Ignoring the wrapper.thread_dump_control_code property because it must be in the range 128-255 or 0."));
    }

    return FALSE;
}
#endif

int validateTimeout(const TCHAR* propertyName, int value, int minValue) {
    int okValue;
    if (value <= 0) {
        okValue = 0;
    } else if (value > WRAPPER_TIMEOUT_MAX) {
        okValue = WRAPPER_TIMEOUT_MAX;
    } else if (value < minValue) {
        okValue = minValue;
    } else {
        okValue = value;
    }

    if (okValue != value) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN,
            TEXT("The value of %s must be in the range %d to %d seconds (%d days), or 0 to disable.  Changing to %d."),
            propertyName, minValue, WRAPPER_TIMEOUT_MAX, WRAPPER_TIMEOUT_MAX / 86400, okValue);
    }

    return okValue;
}

void wrapperLoadHostName() {
    char hostName[80];
    TCHAR* hostName2;
#ifdef UNICODE
    int len;
#endif

    if (gethostname(hostName, sizeof(hostName))) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN, TEXT("Unable to obtain host name. %s"),
            getLastErrorText());
    } else {
#ifdef UNICODE
 #ifdef WIN32
        len = MultiByteToWideChar(CP_OEMCP, 0, hostName, -1, NULL, 0);
        if (len <= 0) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN,
                TEXT("Invalid multibyte sequence in port address \"%s\" : %s"), hostName, getLastErrorText());
            return;
        }

        hostName2 = malloc(sizeof(LPWSTR) * (len + 1));
        if (!hostName2) {
            outOfMemory(TEXT("LHN"), 1);
            return;
        }
        MultiByteToWideChar(CP_OEMCP,0, hostName, -1, hostName2, len + 1);
 #else
        len = mbstowcs(NULL, hostName, MBSTOWCS_QUERY_LENGTH);
        if (len == (size_t)-1) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN,
                TEXT("Invalid multibyte sequence in port address \"%s\" : %s"), hostName, getLastErrorText());
            return;
        }
        hostName2 = malloc(sizeof(TCHAR) * (len + 1));
        if (!hostName2) {
            outOfMemory(TEXT("LHN"), 2);
            return;
        }
        mbstowcs(hostName2, hostName, len + 1);
        hostName2[len] = TEXT('\0'); /* Avoid bufferflows caused by badly encoded characters. */
 #endif
#else
        /* No conversion needed.  Do an extra malloc here to keep the code simple below. */
        len = _tcslen(hostName);
        hostName2 = malloc(sizeof(TCHAR) * (len + 1));
        if (!hostName2) {
            outOfMemory(TEXT("LHN"), 3);
            return;
        }
        _tcsncpy(hostName2, hostName, len + 1);
#endif

        wrapperData->hostName = malloc(sizeof(TCHAR) * (_tcslen(hostName2) + 1));
        if (!wrapperData->hostName) {
            outOfMemory(TEXT("LHN"), 4);
            free(hostName2);
            return;
        }
        _tcsncpy(wrapperData->hostName, hostName2, _tcslen(hostName2) + 1);

        free(hostName2);
    }
}

/**
 * Resolves an action name into an actionId.
 *
 * @param actionName Action to be resolved.  (Contents of buffer will be converted to upper case.)
 * @param propertyName The name of the property where the action name originated.
 * @param logErrors TRUE if errors should be logged.
 *
 * @return The action Id, or 0 if it was unknown.
 */
int getActionForName(TCHAR *actionName, const TCHAR *propertyName, int logErrors) {
    size_t len;
    size_t i;
    int action;

    /* We need the actionName in upper case. */
    len = _tcslen(actionName);
    for (i = 0; i < len; i++) {
        actionName[i] = _totupper(actionName[i]);
    }

    if (_tcscmp(actionName, TEXT("RESTART")) == 0) {
        action = ACTION_RESTART;
    } else if (_tcscmp(actionName, TEXT("SHUTDOWN")) == 0) {
        action = ACTION_SHUTDOWN;
    } else if (_tcscmp(actionName, TEXT("DUMP")) == 0) {
        action = ACTION_DUMP;
    } else if (_tcscmp(actionName, TEXT("NONE")) == 0) {
        action = ACTION_NONE;
    } else if (_tcscmp(actionName, TEXT("DEBUG")) == 0) {
        action = ACTION_DEBUG;
    } else if (_tcscmp(actionName, TEXT("SUCCESS")) == 0) {
        action = ACTION_SUCCESS;
    } else if (_tcscmp(actionName, TEXT("GC")) == 0) {
        action = ACTION_GC;
    } else if (_tcscmp(actionName, TEXT("PAUSE")) == 0) {
        if (logErrors) {
            log_printf(WRAPPER_SOURCE_WRAPPER, properties->logWarningLogLevel, TEXT("Pause actions require the Standard Edition.  Ignoring action '%s' in the %s property."), actionName, propertyName);
        }
        action = 0;
    } else if (_tcscmp(actionName, TEXT("RESUME")) == 0) {
        if (logErrors) {
            log_printf(WRAPPER_SOURCE_WRAPPER, properties->logWarningLogLevel, TEXT("Resume actions require the Standard Edition.  Ignoring action '%s' in the %s property."), actionName, propertyName);
        }
        action = 0;
    } else if (_tcsstr(actionName, TEXT("USER_")) == actionName) {
        if (logErrors) {
            log_printf(WRAPPER_SOURCE_WRAPPER, properties->logWarningLogLevel, TEXT("User actions require the Professional Edition.  Ignoring action '%s' in the %s property."), actionName, propertyName);
        }
        action = 0;
    } else if (_tcsstr(actionName, TEXT("SUSPEND_TIMEOUTS_")) == actionName) {
        if (logErrors) {
            log_printf(WRAPPER_SOURCE_WRAPPER, properties->logWarningLogLevel, TEXT("Suspend timeouts action requires the Standard Edition.  Ignoring action '%s' in the %s property."), actionName, propertyName);
        }
        action = 0;
    } else if (_tcscmp(actionName, TEXT("RESUME_TIMEOUTS")) == 0) {
        if (logErrors) {
            log_printf(WRAPPER_SOURCE_WRAPPER, properties->logWarningLogLevel, TEXT("Resume timeouts actions require the Standard Edition.  Ignoring action '%s' in the %s property."), actionName, propertyName);
        }
        action = 0;
    } else {
        if (logErrors) {
            log_printf(WRAPPER_SOURCE_WRAPPER, properties->logWarningLogLevel, TEXT("Encountered an unknown action '%s' in the %s property.  Skipping."), actionName, propertyName);
        }
        action = 0;
    }

    return action;
}

/**
 * Parses a list of actions for an action property.
 *
 * @param actionNameList A space separated list of action names.
 * @param propertyName The name of the property where the action name originated.
 *
 * @return an array of integer action ids, or NULL if there were any problems.
 */
int *wrapperGetActionListForNames(const TCHAR *actionNameList, const TCHAR *propertyName) {
    size_t len;
    TCHAR *workBuffer;
    TCHAR *token;
    int actionCount;
    int action;
    int *actionList = NULL;
#if defined(UNICODE) && !defined(WIN32)
    TCHAR *state;
#endif

#ifdef _DEBUG
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_INFO, TEXT("wrapperGetActionListForNames(%s, %s)"), actionNameList, propertyName);
#endif

    /* First get a count of the number of valid actions. */
    len = _tcslen(actionNameList);
    workBuffer = malloc(sizeof(TCHAR) * (len + 1));
    if (!workBuffer) {
        outOfMemory(TEXT("GALFN"), 1);
    } else {
        actionCount = 0;
        _tcsncpy(workBuffer, actionNameList, len + 1);
        token = _tcstok(workBuffer, TEXT(" ,")
#if defined(UNICODE) && !defined(WIN32)
            , &state
#endif
);
        while (token != NULL) {
            action = getActionForName(token, propertyName, TRUE);
            if (action == 0) {
                /* Unknown action */
            } else {
                actionCount++;
            }
#ifdef _DEBUG
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_INFO, TEXT("  action='%s' -> %d"), token, action);
#endif
            token = _tcstok(NULL, TEXT(" ,")
#if defined(UNICODE) && !defined(WIN32)
            , &state
#endif
);
        }
        /* Add ACTION_LIST_END */
        actionCount++;

        /* Create the action list to return. */
        actionList = malloc(sizeof(int) * actionCount);
        if (!actionList) {
            outOfMemory(TEXT("GALFN"), 2);
        } else {
            /* Now actually pull out the actions */
            actionCount = 0;
            _tcsncpy(workBuffer, actionNameList, len + 1);
            token = _tcstok(workBuffer, TEXT(" ,")
#if defined(UNICODE) && !defined(WIN32)
            , &state
#endif
);
            while (token != NULL) {
                action = getActionForName(token, propertyName, FALSE);
                if (action == 0) {
                    /* Unknown action */
                } else {
                    actionList[actionCount] = action;
                    actionCount++;
                }
                token = _tcstok(NULL, TEXT(" ,")
#if defined(UNICODE) && !defined(WIN32)
            , &state
#endif
);
            }
            /* Add ACTION_LIST_END */
            actionList[actionCount] = ACTION_LIST_END;
            actionCount++;

            /* actionList returned, so don't free it. */
        }

        free(workBuffer);
    }

    return actionList;
}

/**
 * Loads in the configuration triggers.
 *
 * @return Returns FALSE if successful, TRUE if there were any problems.
 */
int loadConfigurationTriggers() {
    const TCHAR *prop;
    TCHAR propName[256];
    int i;
    TCHAR **propertyNames;
    TCHAR **propertyValues;
    long unsigned int *propertyIndices;
#ifdef _DEBUG
    int j;
#endif

    /* To support reloading, we need to free up any previously loaded filters. */
    if (wrapperData->outputFilterCount > 0) {
        for (i = 0; i < wrapperData->outputFilterCount; i++) {
            free(wrapperData->outputFilters[i]);
            wrapperData->outputFilters[i] = NULL;
        }
        free(wrapperData->outputFilters);
        wrapperData->outputFilters = NULL;

        if (wrapperData->outputFilterActionLists) {
            for (i = 0; i < wrapperData->outputFilterCount; i++) {
                free(wrapperData->outputFilterActionLists[i]);
                wrapperData->outputFilterActionLists[i] = NULL;
            }
            free(wrapperData->outputFilterActionLists);
            wrapperData->outputFilterActionLists = NULL;
        }

        /* Individual messages are references to property values and are not malloced. */
        free(wrapperData->outputFilterMessages);
        wrapperData->outputFilterMessages = NULL;

        free(wrapperData->outputFilterAllowWildFlags);
        wrapperData->outputFilterAllowWildFlags = NULL;

        free(wrapperData->outputFilterMinLens);
        wrapperData->outputFilterMinLens = NULL;
    }

    wrapperData->outputFilterCount = 0;
    if (getStringProperties(properties, TEXT("wrapper.filter.trigger."), TEXT(""), wrapperData->ignoreSequenceGaps, FALSE, &propertyNames, &propertyValues, &propertyIndices)) {
        /* Failed */
        return TRUE;
    }

    /* Loop over the properties and count how many triggers there are. */
    i = 0;
    while (propertyNames[i]) {
        wrapperData->outputFilterCount++;
        i++;
    }
#if defined(MACOSX)
    wrapperData->outputFilterCount++;
    i++;
#endif

    /* Now that a count is known, allocate memory to hold the filters and actions and load them in. */
    if (wrapperData->outputFilterCount > 0) {
        wrapperData->outputFilters = malloc(sizeof(TCHAR *) * wrapperData->outputFilterCount);
        if (!wrapperData->outputFilters) {
            outOfMemory(TEXT("LC"), 1);
            return TRUE;
        }
        memset(wrapperData->outputFilters, 0, sizeof(TCHAR *) * wrapperData->outputFilterCount);

        wrapperData->outputFilterActionLists = malloc(sizeof(int*) * wrapperData->outputFilterCount);
        if (!wrapperData->outputFilterActionLists) {
            outOfMemory(TEXT("LC"), 2);
            return TRUE;
        }
        memset(wrapperData->outputFilterActionLists, 0, sizeof(int*) * wrapperData->outputFilterCount);

        wrapperData->outputFilterMessages = malloc(sizeof(TCHAR *) * wrapperData->outputFilterCount);
        if (!wrapperData->outputFilterMessages) {
            outOfMemory(TEXT("LC"), 3);
            return TRUE;
        }

        wrapperData->outputFilterAllowWildFlags = malloc(sizeof(int) * wrapperData->outputFilterCount);
        if (!wrapperData->outputFilterAllowWildFlags) {
            outOfMemory(TEXT("LC"), 4);
            return TRUE;
        }
        memset(wrapperData->outputFilterAllowWildFlags, 0, sizeof(int) * wrapperData->outputFilterCount);

        wrapperData->outputFilterMinLens = malloc(sizeof(size_t) * wrapperData->outputFilterCount);
        if (!wrapperData->outputFilterMinLens) {
            outOfMemory(TEXT("LC"), 5);
            return TRUE;
        }
        memset(wrapperData->outputFilterMinLens, 0, sizeof(size_t) * wrapperData->outputFilterCount);

        i = 0;
        while (propertyNames[i]) {
            prop = propertyValues[i];

            wrapperData->outputFilters[i] = malloc(sizeof(TCHAR) * (_tcslen(prop) + 1));
            if (!wrapperData->outputFilters[i]) {
                outOfMemory(TEXT("LC"), 3);
                return TRUE;
            }
            _tcsncpy(wrapperData->outputFilters[i], prop, _tcslen(prop) + 1);

            /* Get the action */
            _sntprintf(propName, 256, TEXT("wrapper.filter.action.%lu"), propertyIndices[i]);
            prop = getStringProperty(properties, propName, TEXT("RESTART"));
            wrapperData->outputFilterActionLists[i] = wrapperGetActionListForNames(prop, propName);

            /* Get the message */
            _sntprintf(propName, 256, TEXT("wrapper.filter.message.%lu"), propertyIndices[i]);
            prop = getStringProperty(properties, propName, NULL);
            wrapperData->outputFilterMessages[i] = (TCHAR *)prop;

            /* Get the wildcard flags. */
            _sntprintf(propName, 256, TEXT("wrapper.filter.allow_wildcards.%lu"), propertyIndices[i]);
            wrapperData->outputFilterAllowWildFlags[i] = getBooleanProperty(properties, propName, FALSE);
            if (wrapperData->outputFilterAllowWildFlags[i]) {
                /* Calculate the minimum text length. */
                wrapperData->outputFilterMinLens[i] = wrapperGetMinimumTextLengthForPattern(wrapperData->outputFilters[i]);
            }

#ifdef _DEBUG
            _tprintf(TEXT("filter #%lu, actions=("), propertyIndices[i]);
            if (wrapperData->outputFilterActionLists[i]) {
                j = 0;
                while (wrapperData->outputFilterActionLists[i][j]) {
                    if (j > 0) {
                        _tprintf(TEXT(","));
                    }
                    _tprintf(TEXT("%d"), wrapperData->outputFilterActionLists[i][j]);
                    j++;
                }
            }
            _tprintf(TEXT("), filter='%s'\n"), wrapperData->outputFilters[i]);
#endif
            i++;
        }

#if defined(MACOSX)
        wrapperData->outputFilters[i] = malloc(sizeof(TCHAR) * (_tcslen(TRIGGER_ADVICE_NIL_SERVER) + 1));
        if (!wrapperData->outputFilters[i]) {
            outOfMemory(TEXT("LC"), 4);
            return TRUE;
        }
        _tcsncpy(wrapperData->outputFilters[i], TRIGGER_ADVICE_NIL_SERVER, _tcslen(TRIGGER_ADVICE_NIL_SERVER) + 1);
        wrapperData->outputFilterActionLists[i] = malloc(sizeof(int) * 2);
        if (!wrapperData->outputFilters[i]) {
            outOfMemory(TEXT("LC"), 5);
            return TRUE;
        }
        wrapperData->outputFilterActionLists[i][0] = ACTION_ADVICE_NIL_SERVER;
        wrapperData->outputFilterActionLists[i][1] = ACTION_LIST_END;
        wrapperData->outputFilterMessages[i] = NULL;
        wrapperData->outputFilterAllowWildFlags[i] = FALSE;
        wrapperData->outputFilterMinLens[i] = 0;
        i++;
#endif
    }
    freeStringProperties(propertyNames, propertyValues, propertyIndices);

    return FALSE;
}

int getBackendTypeForName(const TCHAR *typeName) {
    if (strcmpIgnoreCase(typeName, TEXT("SOCKET")) == 0) {
        return WRAPPER_BACKEND_TYPE_SOCKET;
    } else if (strcmpIgnoreCase(typeName, TEXT("SOCKET_IPv4")) == 0) {
        return WRAPPER_BACKEND_TYPE_SOCKET_V4;
    } else if (strcmpIgnoreCase(typeName, TEXT("SOCKET_IPv6")) == 0) {
        return WRAPPER_BACKEND_TYPE_SOCKET_V6;
    } else if (strcmpIgnoreCase(typeName, TEXT("PIPE")) == 0) {
        return WRAPPER_BACKEND_TYPE_PIPE;
    } else if (strcmpIgnoreCase(typeName, TEXT("AUTO")) == 0) {
        return WRAPPER_BACKEND_TYPE_AUTO;
    } else {
        return WRAPPER_BACKEND_TYPE_UNKNOWN;
    }
}

#define WRAPPER_CONSOLE_INPUT_DISABLED   0
#define WRAPPER_CONSOLE_INPUT_ENABLED    1
#define WRAPPER_CONSOLE_INPUT_AUTO       2

static int getConsoleInputMode() {
    const TCHAR* consoleInput;

    if (getStringProperty(properties, TEXT("wrapper.disable_console_input"), NULL) == NULL) {
        /* Deprecated property not set, so default to 'AUTO'. */
        consoleInput = getStringProperty(properties, TEXT("wrapper.console_input"), TEXT("AUTO"));
    } else {
        /* Default to the value of the deprecated property. */
        consoleInput = getStringProperty(properties, TEXT("wrapper.console_input"), 
            getBooleanProperty(properties, TEXT("wrapper.disable_console_input"), FALSE) ? TEXT("DISABLED") : TEXT("ENABLED"));
    }

    if (strcmpIgnoreCase(consoleInput, TEXT("DISABLED")) == 0) {
        return WRAPPER_CONSOLE_INPUT_DISABLED;
    } else if (strcmpIgnoreCase(consoleInput, TEXT("ENABLED")) == 0) {
        return WRAPPER_CONSOLE_INPUT_ENABLED;
    } else if (strcmpIgnoreCase(consoleInput, TEXT("AUTO")) == 0) {
        return WRAPPER_CONSOLE_INPUT_AUTO;
    } else if (properties->logWarnings) {
        log_printf(WRAPPER_SOURCE_WRAPPER, properties->logWarningLogLevel,
            TEXT("Encountered an invalid value for configuration property %s=%s.  Resolving to %s."),
            TEXT("wrapper.console_input"), consoleInput, TEXT("AUTO"));
    }
    return WRAPPER_CONSOLE_INPUT_AUTO;
}

#ifdef WIN32
/**
 * Get whether or not the current service is configured with required privileges.
 *
 * @return TRUE if the service has required privileges, FALSE otherwise.
 */
int currentServiceHasRequiredPrivileges() {
    static int firstCall = TRUE;
    static int result = FALSE;
    SC_HANDLE schSCManager;
    SC_HANDLE schService;
    DWORD bytesNeeded;

    if (firstCall) {
        schSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);
        if (schSCManager) {
            schService = OpenService(schSCManager, wrapperData->serviceName, WRAPPER_SERVICE_QUERY_STATUS);
            if (schService) {
                if (!QueryServiceConfig2(schService,
                             SERVICE_CONFIG_REQUIRED_PRIVILEGES_INFO,
                             NULL,
                             0,
                             &bytesNeeded)) {
                    if (ERROR_INSUFFICIENT_BUFFER == GetLastError()) {
                        /* When no required privileges are defined, the pmszRequiredPrivileges member of
                         *  SERVICE_REQUIRED_PRIVILEGES_INFO will be NULL, so the required buffer size will 
                         *  be sizeof(SERVICE_REQUIRED_PRIVILEGES_INFO) (8 bits). But technically, an empty
                         *  list of privileges also means no privileges, so add the size of 2 termination
                         *  characters (pmszRequiredPrivileges is a multi-string, i.e. a sequence of
                         *  null-terminated strings, terminated by an empty string (\0)). */
                        if (bytesNeeded > (sizeof(SERVICE_REQUIRED_PRIVILEGES_INFO) + (2 * sizeof(TCHAR)))) {
                            result = TRUE;
                        }
                    }
                }
                CloseHandle(schService);
            }
            CloseHandle(schSCManager);
        }
        firstCall = FALSE;
    }
    return result;
}

/**
 * Remove SeImpersonatePrivilege from the access token of the current process.
 *
 * @return 1 if there is an error, 0 otherwise.
 */
static int removeSeImpersonatePrivilege() {
    HANDLE hToken;
    LUID luid;
    TOKEN_PRIVILEGES tp;
    int errLevel = 0;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        errLevel = 1;
    } else if (!LookupPrivilegeValue(NULL, TEXT("SeImpersonatePrivilege"), &luid)) {
        errLevel = 2;
    } else {
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Luid = luid;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_REMOVED;

        if (!AdjustTokenPrivileges(hToken, FALSE, &tp, 0, NULL, NULL)) {
            errLevel = 3;
        } else if (GetLastError() == ERROR_NOT_ALL_ASSIGNED) {
            /* this is ok */
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Privilege '%s' was not present."), TEXT("SeImpersonatePrivilege"));
        } else {
            /* success */
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Privilege '%s' removed."), TEXT("SeImpersonatePrivilege"));
        }
    }

    /* error */
    if (errLevel > 0) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT("Failed to remove privilege '%s' (0x%d) - %s"), TEXT("SeImpersonatePrivilege"), errLevel, getLastErrorText());
        return 1;
    }
    return 0;
}
#endif

/**
 * Strip the quotes of the specified path before appending to the environment.
 *  On Unix, when using 'export LD_LIBRARY_PATH="my path"', the quotes will be
 *  automatically stripped by the command (spaces in the name are handled
 *  correctly because the paths are separated with a ':' character). setEnv()
 *  won't strip bracing quotes, so lets do it here.
 *  Windows allows to add paths with quotes, but they are ignored at runtime.
 *  => For clarity, use this function to always strip bracing quotes.
 *
 * @param path Path to which quotes should be sripped.
 * @param outPtr - If set, the path will be modified by removing the closing
 *                 quote and outPtr will point to the position after the opening
 *                 quote. If not quoted, the parameter simply points to path.
 *               - If NULL, the path is not processed.
 *
 * @return The length without the quotes.
 */
static size_t libraryPathStripQuotes(TCHAR* path, TCHAR** outPtr) {
    size_t len = _tcslen(path);

    if ((len > 1) && (path[0] == TEXT('"')) && (path[len - 1] == TEXT('"'))) {
        if (outPtr) {
            path[len - 1] = 0;
            *outPtr = path + 1;
        }
        return len - 2;
    }
    if (outPtr) {
        *outPtr = path;
    }
    return len;
}

static int wrapperSetSystemLibrariesPath() {
    static size_t buffOldSkip = 0;
    TCHAR* buffAdd;
    TCHAR* buffOld;
    TCHAR* buffNew;
    TCHAR **propertyNames;
    TCHAR **propertyValues;
    long unsigned int *propertyIndices;
    int i;
    TCHAR* path = NULL;
    size_t pathLen;
    size_t buffAddLen = 0;
    size_t tempLen = 0;
#ifdef WIN32
    TCHAR pathSeparator = TEXT(';');
    const TCHAR* envName = TEXT("PATH");
#else
    TCHAR pathSeparator = TEXT(':');
 #ifdef MACOSX
    const TCHAR* envName = TEXT("DYLD_LIBRARY_PATH");
 #else
    const TCHAR* envName = TEXT("LD_LIBRARY_PATH");
 #endif
#endif
    int result = FALSE;

    if (getStringProperties(properties, TEXT("wrapper.system.library.path."), TEXT(""), wrapperData->ignoreSequenceGaps, FALSE, &propertyNames, &propertyValues, &propertyIndices)) {
        return TRUE;
    }

    for (i = 0; propertyNames[i]; i++) {
        pathLen = libraryPathStripQuotes(propertyValues[i], NULL);
        if (pathLen > 0) {
            buffAddLen += pathLen + 1;
        }
    }

    if (buffAddLen > 0) {
        buffAdd = malloc(sizeof(TCHAR) * (buffAddLen + 1));
        if (!buffAdd) {
            outOfMemory(TEXT("WSSLP"), 1);
            result = TRUE;
        } else {
            buffAdd[0] = 0;

            for (i = 0; propertyNames[i]; i++) {
                pathLen = libraryPathStripQuotes(propertyValues[i], &path);
                if (pathLen > 0) {
                    _tcsncat(buffAdd, path, buffAddLen - tempLen);
                    tempLen += pathLen;

                    buffAdd[tempLen++] = pathSeparator;
                    buffAdd[tempLen] = 0;
                }
            }

#ifdef WIN32
            wrapperCorrectWindowsPath(buffAdd);
#else
            wrapperCorrectNixPath(buffAdd);
#endif

            buffOld = _tgetenv(envName);
            if (!buffOld || (_tcslen(buffOld) <= buffOldSkip)) {
                /* The variable was not set when the Wrapper started - crop the last path separator and simply set the variable with our value. */
                buffAdd[buffAddLen - 1] = 0;
                result = setEnv(envName, buffAdd, ENV_SOURCE_CONFIG);
            } else {
                /* Append our paths to the existing value (buffAdd ends with a path separator). */
                tempLen = _tcslen(buffAdd) + _tcslen(buffOld + buffOldSkip);
                buffNew = malloc(sizeof(TCHAR) * (tempLen + 1));
                if (!buffNew) {
                    outOfMemory(TEXT("WSSLP"), 2);
                    result = TRUE;
                } else {
                    _sntprintf(buffNew, tempLen + 1, TEXT("%s%s"), buffAdd, buffOld + buffOldSkip);
                    buffNew[tempLen] = 0;
                    result = setEnv(envName, buffNew, ENV_SOURCE_CONFIG);
                    free(buffNew);
                }
            }
#if !defined(WIN32) && defined(UNICODE)
            free(buffOld);
#endif
            if (!result) {
                /* Environment was set successfully - remember the number of characters added so we can skip them in case we need to reset the variable later. */
                buffOldSkip = buffAddLen;
            }
            free(buffAdd);
        }
    }
    freeStringProperties(propertyNames, propertyValues, propertyIndices);

    return result;
}

static int checkStripQuotes() {
    TCHAR **propertyNames;
    int *propertyValues;
    int i = 0;
    int result = FALSE;

    if (getBooleanProperties(properties, TEXT("wrapper."), TEXT(".stripquotes"), TRUE, TRUE, &propertyNames, &propertyValues, NULL, FALSE)) {
        result = TRUE;
    } else {
        if (propertyNames[0]) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("The following '.stripquotes' properties were found:"));
            while (propertyNames[i]) {
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("  %s"), propertyNames[i]);
                i++;
            }
            if (wrapperData->isAdviserEnabled) {
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ADVICE, TEXT(""));
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ADVICE, TEXT(
                    "--------------------------------------------------------------------"));
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ADVICE, TEXT("The quote stripping mode was used by older versions of the Wrapper\n  to remove unescaped quotes when running on UNIX machines, and is\n  no longer valid. The current Wrapper version handles quotes,\n  spaces and other special characters without the need of this mode.\n  While removing the above properties from your configuration file,\n  be sure to also update the values they apply to. Quotes and\n  backslashes no longer need to be escaped."));
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ADVICE, TEXT(
                    "--------------------------------------------------------------------"));
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ADVICE, TEXT(""));
            }
            result = TRUE;
        }
        freeBooleanProperties(propertyNames, propertyValues, NULL);
    }
    return result;
}

/**
 * Return FALSE if successful, TRUE if there were problems.
 */
int loadConfiguration() {
    TCHAR propName[256];
    const TCHAR* val;
    int startupDelay;
    const TCHAR* format;
#ifndef WIN32
    int terminalPgid;
    struct stat sb1;
    struct stat sb2;
#endif

    if (wrapperLoadLoggingProperties(FALSE)) {
        return TRUE;
    }
    
    /* Decide on the error exit code */
    getConfiguredErrorExitCode(FALSE);

    /* Decide on the backend type to use. */    
    val = getStringProperty(properties, TEXT("wrapper.backend.type"), TEXT("AUTO"));
    wrapperData->backendTypeBit = 0;
    wrapperData->backendTypeConfiguredBits = getBackendTypeForName(val);
    if (wrapperData->backendTypeConfiguredBits == WRAPPER_BACKEND_TYPE_UNKNOWN) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Unknown value for wrapper.backend.type: %s. Setting it to AUTO."), val);
        wrapperData->backendTypeConfiguredBits = WRAPPER_BACKEND_TYPE_AUTO;
    }

    /* Decide whether the parameters should be passed via the backend or command line. */
    wrapperData->useBackendParameters = getBooleanProperty(properties, TEXT("wrapper.app.parameter.backend"), FALSE);

    /* Decide whether the classpath should be passed via the environment. */
    wrapperData->environmentClasspath = getBooleanProperty(properties, TEXT("wrapper.java.classpath.use_environment"), FALSE);

    /* Decide how sequence gaps should be handled before any other properties are loaded. */
    wrapperData->ignoreSequenceGaps = getBooleanProperty(properties, TEXT("wrapper.ignore_sequence_gaps"), FALSE);

    /* To make configuration reloading work correctly with changes to the log file,
     *  it needs to be closed here. */
    closeLogfile();

    /* Maintain the logger just in case we wrote any queued errors. */
    maintainLogger();
    /* Because the first call could cause errors as well, do it again to clear them out.
     *  This is only a one-time thing on startup as we test the new logfile configuration. */
    maintainLogger();

    /* Initialize some values not loaded */
    wrapperData->exitCode = 0;

    if (checkStripQuotes()) {
        return TRUE;
    }

    /* Decide whether all the configuration (including sensitive properties) should be shared with the JVM. */
    val = getStringProperty(properties, TEXT("wrapper.java.share_configuration"), TEXT("DEFAULT"));
    if (strcmpIgnoreCase(val, TEXT("ALL")) == 0) {
        wrapperData->shareAllConfiguration = TRUE;
    } else {
        if (strcmpIgnoreCase(val, TEXT("DEFAULT")) != 0) {
            log_printf(WRAPPER_SOURCE_WRAPPER, properties->logWarningLogLevel,
                TEXT("Encountered an invalid value for configuration property %s=%s.  Resolving to %s."),
                TEXT("wrapper.java.share_configuration"), val, TEXT("DEFAULT"));
        }
        wrapperData->shareAllConfiguration = FALSE;
    }

    updateStringValue(&wrapperData->portAddress, getStringProperty(properties, TEXT("wrapper.port.address"), NULL));
    /* Get the port. The int will wrap within the 0-65535 valid range, so no need to test the value. */
    wrapperData->port = getIntProperty(properties, TEXT("wrapper.port"), 0);
    wrapperData->portMin = getIntProperty(properties, TEXT("wrapper.port.min"), 32000);
    if ((wrapperData->portMin < 1) || (wrapperData->portMin > 65535)) {
        wrapperData->portMin = 32000;
        log_printf(WRAPPER_SOURCE_WRAPPER, properties->logWarningLogLevel,
            TEXT("%s must be in the range %d to %d.  Changing to %d."), TEXT("wrapper.port.min"), 1, 65535, wrapperData->portMin);
    }
    wrapperData->portMax = getIntProperty(properties, TEXT("wrapper.port.max"), 32999);
    if ((wrapperData->portMax < 1) || (wrapperData->portMax > 65535)) {
        wrapperData->portMax = __min(wrapperData->portMin + 999, 65535);
        log_printf(WRAPPER_SOURCE_WRAPPER, properties->logWarningLogLevel,
            TEXT("%s must be in the range %d to %d.  Changing to %d."), TEXT("wrapper.port.max"), 1, 65535, wrapperData->portMax);
    } else if (wrapperData->portMax < wrapperData->portMin) {
        wrapperData->portMax = __min(wrapperData->portMin + 999, 65535);
        log_printf(WRAPPER_SOURCE_WRAPPER, properties->logWarningLogLevel,
            TEXT("%s must be greater than or equal to %s.  Changing to %d."), TEXT("wrapper.port.max"), TEXT("wrapper.port.min"), wrapperData->portMax);
    }

    /* Get the port for the JVM side of the socket. */
    wrapperData->jvmPort = getIntProperty(properties, TEXT("wrapper.jvm.port"), -1);
    if (wrapperData->jvmPort > 0) {
        if (wrapperData->jvmPort == wrapperData->port) {
            wrapperData->jvmPort = -1;
            log_printf(WRAPPER_SOURCE_WRAPPER, properties->logWarningLogLevel,
                TEXT("wrapper.jvm.port must not equal wrapper.port.  Changing to the default."));
        }
    }
    wrapperData->jvmPortMin = getIntProperty(properties, TEXT("wrapper.jvm.port.min"), 31000);
    if ((wrapperData->jvmPortMin < 1) || (wrapperData->jvmPortMin > 65535)) {
        wrapperData->jvmPortMin = 31000;
        log_printf(WRAPPER_SOURCE_WRAPPER, properties->logWarningLogLevel,
            TEXT("%s must be in the range %d to %d.  Changing to %d."), TEXT("wrapper.jvm.port.min"), 1, 65535, wrapperData->jvmPortMin);
    }
    wrapperData->jvmPortMax = getIntProperty(properties, TEXT("wrapper.jvm.port.max"), 31999);
    if ((wrapperData->jvmPortMax < 1) || (wrapperData->jvmPortMax > 65535)) {
        wrapperData->jvmPortMax = __min(wrapperData->jvmPortMin + 999, 65535);
        log_printf(WRAPPER_SOURCE_WRAPPER, properties->logWarningLogLevel,
            TEXT("%s must be in the range %d to %d.  Changing to %d."), TEXT("wrapper.jvm.port.max"), 1, 65535, wrapperData->jvmPortMax);
    } else if (wrapperData->jvmPortMax < wrapperData->jvmPortMin) {
        wrapperData->jvmPortMax = __min(wrapperData->jvmPortMin + 999, 65535);
        log_printf(WRAPPER_SOURCE_WRAPPER, properties->logWarningLogLevel,
            TEXT("%s must be greater than or equal to %s.  Changing to %d."), TEXT("wrapper.jvm.port.max"), TEXT("wrapper.jvm.port.min"), wrapperData->jvmPortMax);
    }

    /* These properties are not documented. */
    wrapperData->javaQueryTimeout = getIntProperty(properties, TEXT("wrapper.java.query.timeout"), DEFAULT_JAVA_QUERY_TIMEOUT) * 1000;
    if (wrapperData->javaQueryTimeout == 1000) {
        wrapperData->javaQueryEvaluationTimeout = 500;
    } else if ((wrapperData->javaQueryTimeout == 2000) || (wrapperData->javaQueryTimeout == 3000)) {
        wrapperData->javaQueryEvaluationTimeout = 1000;
    } else {
        wrapperData->javaQueryEvaluationTimeout = getIntProperty(properties, TEXT("wrapper.java.query.evaluation.timeout"), DEFAULT_JAVA_QUERY_EVALUATION_TIMEOUT) * 1000;
        if (wrapperData->javaQueryEvaluationTimeout <= 1000) {
            wrapperData->javaQueryEvaluationTimeout = 1000;
        } else if ((wrapperData->javaQueryTimeout == 0) || (wrapperData->javaQueryTimeout >= 22000)) {
            wrapperData->javaQueryEvaluationTimeout = __min(wrapperData->javaQueryEvaluationTimeout, 20000);
        } else {
            wrapperData->javaQueryEvaluationTimeout = __min(wrapperData->javaQueryEvaluationTimeout, wrapperData->javaQueryTimeout - 2000);
        }
    }

    wrapperData->javaQueryLogLevel = getLogLevelForName(getStringProperty(properties, TEXT("wrapper.java.query.loglevel"), TEXT("DEBUG")));
    if ((wrapperData->javaQueryLogLevel >= LEVEL_NONE) || (wrapperData->javaQueryLogLevel == LEVEL_UNKNOWN)) {
        /* To help support, force at least DEBUG.  It also simplifies the comparison logic otherwise a special case would be needed for LEVEL_NONE which is actually the highest level. */
        wrapperData->javaQueryLogLevel = LEVEL_DEBUG;
    }

    wrapperData->javaVersionTimeout = getIntProperty(properties, TEXT("wrapper.java.version.timeout"), wrapperData->javaQueryTimeout / 1000) * 1000;

    wrapperData->printJVMVersion = getBooleanProperty(properties, TEXT("wrapper.java.version.output"), wrapperData->javaQueryLogLevel >= LEVEL_INFO ? TRUE : FALSE);

    /* Get the java command log level. */
    wrapperData->commandLogLevel = getLogLevelForName(
        getStringProperty(properties, TEXT("wrapper.java.command.loglevel"), TEXT("DEBUG")));
    if ((wrapperData->commandLogLevel >= LEVEL_NONE) || (wrapperData->commandLogLevel == LEVEL_UNKNOWN)) {
        /* Should never be possible to completely disable the java command as this would make it very difficult to support. */
        wrapperData->commandLogLevel = LEVEL_DEBUG;
    }

    /* Get the java command print format. */
#ifdef WIN32
    format = getStringProperty(properties, TEXT("wrapper.java.command.windows.format"), TEXT("CMD"));
    if (strcmpIgnoreCase(format, TEXT("POWERSHELL")) == 0) {
        wrapperData->jvmCommandPrintFormat = COMMAND_FORMAT_POWERSHELL;
    } else {
        wrapperData->jvmCommandPrintFormat = COMMAND_FORMAT_CMD;
    }
#else
    format = getStringProperty(properties, TEXT("wrapper.java.command.unix.format"), TEXT("LINE"));
    if (strcmpIgnoreCase(format, TEXT("ARRAY")) == 0) {
        wrapperData->jvmCommandPrintFormat = COMMAND_FORMAT_ARRAY;
    } else {
        wrapperData->jvmCommandPrintFormat = COMMAND_FORMAT_LINE;
    }
#endif
    wrapperData->jvmCommandShowBackendProps = getBooleanProperty(properties, TEXT("wrapper.java.command.show_backend_properties"), FALSE);

#ifdef WIN32
    /* Unless wrapper.java.monitor=LAUNCHED is specified, always manage the Java process if there is a redirection. */
    val = getStringProperty(properties, TEXT("wrapper.java.monitor"), TEXT("JAVA"));
    if (strcmpIgnoreCase(val, TEXT("LAUNCHED")) == 0) {
        wrapperData->monitorLaunched = TRUE;
    } else {
        if (strcmpIgnoreCase(val, TEXT("JAVA")) != 0) {
            log_printf(WRAPPER_SOURCE_WRAPPER, properties->logWarningLogLevel, TEXT("Encountered an invalid value for configuration property %s=%s.  Resolving to %s."), TEXT("wrapper.java.monitor"), val, TEXT("JAVA"));
        }
        wrapperData->monitorLaunched = FALSE;
    }
#endif
    wrapperData->monitorRedirectLogLevel = getLogLevelForName(getStringProperty(properties, TEXT("wrapper.java.monitor.redirect.loglevel"), TEXT("WARN")));
    if ((wrapperData->monitorRedirectLogLevel >= LEVEL_NONE) || (wrapperData->monitorRedirectLogLevel == LEVEL_UNKNOWN)) {
        /* Always at least show a debug message for support. */
        wrapperData->monitorRedirectLogLevel = LEVEL_DEBUG;
    }
    
    /* Should we detach the JVM on startup. */
    if (wrapperData->isConsole) {
        wrapperData->detachStarted = getBooleanProperty(properties, TEXT("wrapper.jvm_detach_started"), FALSE);
    }

    /* Get the use system time flag. */
    if (!wrapperData->configured) {
        wrapperData->useSystemTime = getBooleanProperty(properties, TEXT("wrapper.use_system_time"), FALSE);

        wrapperData->logBufferGrowth = getBooleanProperty(properties, TEXT("wrapper.log_buffer_growth"), FALSE);
        setLogBufferGrowth(wrapperData->logBufferGrowth);

#ifdef WIN32
        /* Get the use javaio buffer size. */
        wrapperData->javaIOBufferSize = getIntProperty(properties, TEXT("wrapper.javaio.buffer_size"), WRAPPER_JAVAIO_BUFFER_SIZE_DEFAULT);
        if (wrapperData->javaIOBufferSize == WRAPPER_JAVAIO_BUFFER_SIZE_SYSTEM_DEFAULT) {
            /* Ok. System default buffer size. */
        } else if (wrapperData->javaIOBufferSize < WRAPPER_JAVAIO_BUFFER_SIZE_MIN) {
            wrapperData->javaIOBufferSize = WRAPPER_JAVAIO_BUFFER_SIZE_MIN;
            log_printf(WRAPPER_SOURCE_WRAPPER, properties->logWarningLogLevel,
                TEXT("%s must be in the range %d to %d or %d.  Changing to %d."), TEXT("wrapper.javaio.buffer_size"), WRAPPER_JAVAIO_BUFFER_SIZE_MIN, WRAPPER_JAVAIO_BUFFER_SIZE_MAX, WRAPPER_JAVAIO_BUFFER_SIZE_SYSTEM_DEFAULT, wrapperData->javaIOBufferSize);
        } else if (wrapperData->javaIOBufferSize > WRAPPER_JAVAIO_BUFFER_SIZE_MAX) {
            wrapperData->javaIOBufferSize = WRAPPER_JAVAIO_BUFFER_SIZE_MAX;
            log_printf(WRAPPER_SOURCE_WRAPPER, properties->logWarningLogLevel,
                TEXT("%s must be in the range %d to %d or %d.  Changing to %d."), TEXT("wrapper.javaio.buffer_size"), WRAPPER_JAVAIO_BUFFER_SIZE_MIN, WRAPPER_JAVAIO_BUFFER_SIZE_MAX, WRAPPER_JAVAIO_BUFFER_SIZE_SYSTEM_DEFAULT, wrapperData->javaIOBufferSize);
        }
#endif

        /* Get the use javaio thread flag. */
        wrapperData->useJavaIOThread = getBooleanProperty(properties, TEXT("wrapper.javaio.use_thread"), getBooleanProperty(properties, TEXT("wrapper.use_javaio_thread"), FALSE));

        /* Decide whether or not a mutex should be used to protect the tick timer. */
        wrapperData->useTickMutex = getBooleanProperty(properties, TEXT("wrapper.use_tick_mutex"), FALSE);
    }
    
    /* Get the timer thresholds. Properties are in seconds, but internally we use ticks. */
    wrapperData->timerFastThreshold = getIntProperty(properties, TEXT("wrapper.timer_fast_threshold"), WRAPPER_TIMER_FAST_THRESHOLD * WRAPPER_TICK_MS / 1000) * 1000 / WRAPPER_TICK_MS;
    wrapperData->timerSlowThreshold = getIntProperty(properties, TEXT("wrapper.timer_slow_threshold"), WRAPPER_TIMER_SLOW_THRESHOLD * WRAPPER_TICK_MS / 1000) * 1000 / WRAPPER_TICK_MS;

    /* Load the name of the native library to be loaded. */
    wrapperData->nativeLibrary = getStringProperty(properties, TEXT("wrapper.native_library"), TEXT("wrapper"));

    /* Get the append PATH to library path flag. */
    wrapperData->libraryPathAppendPath = getBooleanProperty(properties, TEXT("wrapper.java.library.path.append_system_path"), FALSE);

    /* Load secondary libraries which are not directly loaded by Java. */
    if (wrapperSetSystemLibrariesPath()) {
        return TRUE;
    }

    /* Get the state output status. */
    wrapperData->isStateOutputEnabled = getBooleanProperty(properties, TEXT("wrapper.state_output"), FALSE);
    
    /* Get the mode used to print the state output. */
    wrapperData->stateOutputMode = getStateOutputModeForName(getStringProperty(properties, TEXT("wrapper.state_output.mode"), TEXT("DEFAULT")));

#ifdef WIN32
    /* Get the message output status. */
    wrapperData->isMessageOutputEnabled = getBooleanProperty(properties, TEXT("wrapper.message_output"), FALSE);
#endif

    /* Get the javaio output status. */
    wrapperData->isJavaIOOutputEnabled = getBooleanProperty(properties, TEXT("wrapper.javaio_output"), FALSE);

    /* Get the tick output status. */
    wrapperData->isTickOutputEnabled = getBooleanProperty(properties, TEXT("wrapper.tick_output"), FALSE);

    /* Get the loop debug output status. */
    wrapperData->isLoopOutputEnabled = getBooleanProperty(properties, TEXT("wrapper.loop_output"), FALSE);

    /* Get the sleep debug output status. */
    wrapperData->isSleepOutputEnabled = getBooleanProperty(properties, TEXT("wrapper.sleep_output"), FALSE);


    /* Get the disable tests flag. */
    wrapperData->isTestsDisabled = getBooleanProperty(properties, TEXT("wrapper.disable_tests"), FALSE);

    /* Get the shutdown hook status */
    wrapperData->isShutdownHookDisabled = getBooleanProperty(properties, TEXT("wrapper.disable_shutdown_hook"), FALSE);
    
    /* Get the forced shutdown flag status. */
    wrapperData->isForcedShutdownDisabled = getBooleanProperty(properties, TEXT("wrapper.disable_forced_shutdown"), FALSE);
    wrapperData->forcedShutdownDelay = getIntProperty(properties, TEXT("wrapper.forced_shutdown.delay"), 2);

    /* Get the startup delay. */
    startupDelay = getIntProperty(properties, TEXT("wrapper.startup.delay"), 0);
    wrapperData->startupDelayConsole = getIntProperty(properties, TEXT("wrapper.startup.delay.console"), startupDelay);
    if (wrapperData->startupDelayConsole < 0) {
        wrapperData->startupDelayConsole = 0;
    }
    wrapperData->startupDelayService = getIntProperty(properties, TEXT("wrapper.startup.delay.service"), startupDelay);
    if (wrapperData->startupDelayService < 0) {
        wrapperData->startupDelayService = 0;
    }

    /* Get the restart delay. */
    wrapperData->restartDelay = getIntProperty(properties, TEXT("wrapper.restart.delay"), 5);
    if (wrapperData->restartDelay < 0) {
        wrapperData->restartDelay = 0;
    }

    /* Get the flag which decides whether or not configuration should be reloaded on JVM restart. */
    wrapperData->restartReloadConf = getBooleanProperty(properties, TEXT("wrapper.restart.reload_configuration"), FALSE);

    /* Get the disable restart flag */
    wrapperData->isRestartDisabled = getBooleanProperty(properties, TEXT("wrapper.disable_restarts"), FALSE);
    wrapperData->isAutoRestartDisabled = getBooleanProperty(properties, TEXT("wrapper.disable_restarts.automatic"), wrapperData->isRestartDisabled);

    /* Get the flag which decides whether or not a JVM is allowed to be launched. */
    wrapperData->runWithoutJVM = getBooleanProperty(properties, TEXT("wrapper.test.no_jvm"), FALSE); 

    wrapperData->pidLogLevel = getLogLevelForName(getStringProperty(properties, TEXT("wrapper.java.pid.loglevel"), TEXT("DEBUG")));
    
    /* Event loop sleep rule configuration. */
    wrapperData->mainLoopStepCycles = __max(1, __min(100, getIntProperty(properties, TEXT("wrapper.javaio.idle.sleep_step_cycles"), 5)));
    wrapperData->mainLoopSleepStepMs = __max(1, __min(1000, getIntProperty(properties, TEXT("wrapper.javaio.idle.sleep_step_ms"), 1)));
    wrapperData->mainLoopMaxSleepMs = __max(10, __min(1000, getIntProperty(properties, TEXT("wrapper.javaio.idle.sleep_max_ms"), 10)));

    /* Get the timeout settings */
    wrapperData->cpuTimeout = getIntProperty(properties, TEXT("wrapper.cpu.timeout"), 10);
    wrapperData->startupTimeout = getIntProperty(properties, TEXT("wrapper.startup.timeout"), 30);
    wrapperData->pingTimeout = getIntProperty(properties, TEXT("wrapper.ping.timeout"), 30);
    if (wrapperData->pingActionList) {
        free(wrapperData->pingActionList);
    }
    wrapperData->pingActionList = wrapperGetActionListForNames(getStringProperty(properties, TEXT("wrapper.ping.timeout.action"), TEXT("RESTART")), TEXT("wrapper.ping.timeout.action"));
    wrapperData->pingAlertThreshold = getIntProperty(properties, TEXT("wrapper.ping.alert.threshold"), __max(1, wrapperData->pingTimeout / 4));
    wrapperData->pingAlertLogLevel = getLogLevelForName(getStringProperty(properties, TEXT("wrapper.ping.alert.loglevel"), TEXT("STATUS")));
    wrapperData->pingInterval = getIntProperty(properties, TEXT("wrapper.ping.interval"), 5);
    wrapperData->pingIntervalLogged = getIntProperty(properties, TEXT("wrapper.ping.interval.logged"), 1);
    wrapperData->shutdownTimeout = getIntProperty(properties, TEXT("wrapper.shutdown.timeout"), 30);
    wrapperData->jvmExitTimeout = getIntProperty(properties, TEXT("wrapper.jvm_exit.timeout"), 15);
    wrapperData->jvmCleanupTimeout = getIntProperty(properties, TEXT("wrapper.jvm_cleanup.timeout"), 10);
    wrapperData->jvmTerminateTimeout = getIntProperty(properties, TEXT("wrapper.jvm_terminate.timeout"), 0);

    wrapperData->cpuTimeout = validateTimeout(TEXT("wrapper.cpu.timeout"), wrapperData->cpuTimeout, 1);
    wrapperData->startupTimeout = validateTimeout(TEXT("wrapper.startup.timeout"), wrapperData->startupTimeout, 1);
    wrapperData->pingTimeout = validateTimeout(TEXT("wrapper.ping.timeout"), wrapperData->pingTimeout, 1);
    wrapperData->shutdownTimeout = validateTimeout(TEXT("wrapper.shutdown.timeout"), wrapperData->shutdownTimeout, 1);
    wrapperData->jvmExitTimeout = validateTimeout(TEXT("wrapper.jvm_exit.timeout"), wrapperData->jvmExitTimeout, 1);
    wrapperData->jvmTerminateTimeout = validateTimeout(TEXT("wrapper.jvm_terminate.timeout"), wrapperData->jvmTerminateTimeout, 5);
    wrapperData->jvmCleanupTimeout = validateTimeout(TEXT("wrapper.jvm_cleanup.timeout"), wrapperData->jvmCleanupTimeout, 1);

    if (wrapperData->pingInterval < 1) {
        wrapperData->pingInterval = 1;
        log_printf(WRAPPER_SOURCE_WRAPPER, properties->logWarningLogLevel,
            TEXT("The value of %s must be at least %d second(s).  Changing to %d."), TEXT("wrapper.ping.interval"), 1, wrapperData->pingInterval);
    } else if (wrapperData->pingInterval > 3600) {
        wrapperData->pingInterval = 3600;
        log_printf(WRAPPER_SOURCE_WRAPPER, properties->logWarningLogLevel,
            TEXT("wrapper.ping.interval must be less than or equal to 1 hour (3600 seconds).  Changing to 3600."));
    }
    if (wrapperData->pingIntervalLogged < 1) {
        wrapperData->pingIntervalLogged = 1;
        log_printf(WRAPPER_SOURCE_WRAPPER, properties->logWarningLogLevel,
            TEXT("The value of %s must be at least %d second(s).  Changing to %d."), TEXT("wrapper.ping.interval.logged"), 1, wrapperData->pingIntervalLogged);
    } else if (wrapperData->pingIntervalLogged > 86400) {
        wrapperData->pingIntervalLogged = 86400;
        log_printf(WRAPPER_SOURCE_WRAPPER, properties->logWarningLogLevel,
            TEXT("wrapper.ping.interval.logged must be less than or equal to 1 day (86400 seconds).  Changing to 86400."));
    }

    if ((wrapperData->pingTimeout > 0) && (wrapperData->pingTimeout < wrapperData->pingInterval + 5)) {
        wrapperData->pingTimeout = wrapperData->pingInterval + 5;
        log_printf(WRAPPER_SOURCE_WRAPPER, properties->logWarningLogLevel,
            TEXT("wrapper.ping.timeout must be at least 5 seconds longer than wrapper.ping.interval.  Changing to %d."), wrapperData->pingTimeout);
    }
    if (wrapperData->pingAlertThreshold <= 0) {
        /* Ping Alerts disabled. */
        wrapperData->pingAlertThreshold = 0;
    } else if ((wrapperData->pingTimeout > 0) && (wrapperData->pingAlertThreshold > wrapperData->pingTimeout)) {
        wrapperData->pingAlertThreshold = wrapperData->pingTimeout;
        log_printf(WRAPPER_SOURCE_WRAPPER, properties->logWarningLogLevel,
            TEXT("wrapper.ping.alert.threshold must be less than or equal to the value of wrapper.ping.timeout (%d seconds).  Changing to %d."),
            wrapperData->pingInterval, wrapperData->pingTimeout);
    }
    if (wrapperData->cpuTimeout > 0) {
        /* Make sure that the timeouts are all longer than the cpu timeout. */
        if ((wrapperData->startupTimeout > 0) && (wrapperData->startupTimeout < wrapperData->cpuTimeout)) {
            log_printf(WRAPPER_SOURCE_WRAPPER, properties->logWarningLogLevel,
                TEXT("CPU timeout detection may not operate correctly during startup because wrapper.cpu.timeout is not smaller than wrapper.startup.timeout."));
        }
        if ((wrapperData->pingTimeout > 0) && (wrapperData->pingTimeout < wrapperData->cpuTimeout)) {
            log_printf(WRAPPER_SOURCE_WRAPPER, properties->logWarningLogLevel,
                TEXT("CPU timeout detection may not operate correctly because wrapper.cpu.timeout is not smaller than wrapper.ping.timeout."));
        }
        if ((wrapperData->shutdownTimeout > 0) && (wrapperData->shutdownTimeout < wrapperData->cpuTimeout)) {
            log_printf(WRAPPER_SOURCE_WRAPPER, properties->logWarningLogLevel,
                TEXT("CPU timeout detection may not operate correctly during shutdown because wrapper.cpu.timeout is not smaller than wrapper.shutdown.timeout."));
        }
        /* jvmExit timeout can be shorter than the cpu timeout. */
    }

    /* Load properties controlling the number times the JVM can be restarted. */
    wrapperData->maxFailedInvocations = getIntProperty(properties, TEXT("wrapper.max_failed_invocations"), 5);
    if (wrapperData->maxFailedInvocations < 1) {
        wrapperData->maxFailedInvocations = 1;
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR,
            TEXT("The value of %s must be at least %d.  Changing to %d."), TEXT("wrapper.max_failed_invocations"), 1, wrapperData->maxFailedInvocations);
    }
    wrapperData->successfulInvocationTime = getIntProperty(properties, TEXT("wrapper.successful_invocation_time"), 300);
    if (wrapperData->successfulInvocationTime < 1) {
        wrapperData->successfulInvocationTime = 1;
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR,
            TEXT("The value of %s must be at least %d second(s).  Changing to %d."), TEXT("wrapper.successful_invocation_time"), 1, wrapperData->successfulInvocationTime);
    }

    /* TRUE if the JVM should be asked to dump its state when it fails to halt on request. */
    wrapperData->requestThreadDumpOnFailedJVMExit = getBooleanProperty(properties, TEXT("wrapper.request_thread_dump_on_failed_jvm_exit"), FALSE);
    wrapperData->requestThreadDumpOnFailedJVMExitDelay = getIntProperty(properties, TEXT("wrapper.request_thread_dump_on_failed_jvm_exit.delay"), 5);
    if (wrapperData->requestThreadDumpOnFailedJVMExitDelay < 1) {
        wrapperData->requestThreadDumpOnFailedJVMExitDelay = 1;
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR,
            TEXT("The value of %s must be at least %d second(s).  Changing to %d."), TEXT("wrapper.request_thread_dump_on_failed_jvm_exit.delay"), 1, wrapperData->requestThreadDumpOnFailedJVMExitDelay);
    }

    /* Load the output filters. */
    if (loadConfigurationTriggers()) {
        return TRUE;
    }

    /** Get the pid files if any.  May be NULL */
    if (!wrapperData->configured) {
        updateStringValue(&wrapperData->pidFilename, getFileSafeStringProperty(properties, TEXT("wrapper.pidfile"), NULL));
        wrapperCorrectWindowsPath(wrapperData->pidFilename);
    }
    wrapperData->pidFileStrict = getBooleanProperty(properties, TEXT("wrapper.pidfile.strict"), FALSE);
    
    updateStringValue(&wrapperData->javaPidFilename, getFileSafeStringProperty(properties, TEXT("wrapper.java.pidfile"), NULL));
    wrapperCorrectWindowsPath(wrapperData->javaPidFilename);

    /** Get the lock file if any.  May be NULL */
    if (!wrapperData->configured) {
        updateStringValue(&wrapperData->lockFilename, getFileSafeStringProperty(properties, TEXT("wrapper.lockfile"), NULL));
        wrapperCorrectWindowsPath(wrapperData->lockFilename);
    }

    /** Get the java id file.  May be NULL */
    updateStringValue(&wrapperData->javaIdFilename, getFileSafeStringProperty(properties, TEXT("wrapper.java.idfile"), NULL));
    wrapperCorrectWindowsPath(wrapperData->javaIdFilename);

    /** Get the status files if any.  May be NULL */
    if (!wrapperData->configured) {
        updateStringValue(&wrapperData->statusFilename, getFileSafeStringProperty(properties, TEXT("wrapper.statusfile"), NULL));
        wrapperCorrectWindowsPath(wrapperData->statusFilename);
    }
    updateStringValue(&wrapperData->javaStatusFilename, getFileSafeStringProperty(properties, TEXT("wrapper.java.statusfile"), NULL));
    wrapperCorrectWindowsPath(wrapperData->javaStatusFilename);

    /** Get the command file if any. May be NULL */
    updateStringValue(&wrapperData->commandFilename, getFileSafeStringProperty(properties, TEXT("wrapper.commandfile"), NULL));
    wrapperCorrectWindowsPath(wrapperData->commandFilename);
    wrapperData->commandFileTests = getBooleanProperty(properties, TEXT("wrapper.commandfile.enable_tests"), FALSE);

    /** Get the interval at which the command file will be polled. */
    wrapperData->commandPollInterval = propIntMin(propIntMax(getIntProperty(properties, TEXT("wrapper.commandfile.poll_interval"), getIntProperty(properties, TEXT("wrapper.command.poll_interval"), 5)), 1), 3600);

    /** Get the anchor file if any.  May be NULL */
    if (!wrapperData->configured) {
        updateStringValue(&wrapperData->anchorFilename, getFileSafeStringProperty(properties, TEXT("wrapper.anchorfile"), NULL));
        wrapperCorrectWindowsPath(wrapperData->anchorFilename);
    }

    /** Get the interval at which the anchor file will be polled. */
    wrapperData->anchorPollInterval = propIntMin(propIntMax(getIntProperty(properties, TEXT("wrapper.anchorfile.poll_interval"), getIntProperty(properties, TEXT("wrapper.anchor.poll_interval"), 5)), 1), 3600);

    /** Flag controlling whether or not system signals should be ignored. */
    val = getStringProperty(properties, TEXT("wrapper.ignore_signals"), TEXT("FALSE"));
    if ( ( strcmpIgnoreCase( val, TEXT("TRUE") ) == 0 ) || ( strcmpIgnoreCase( val, TEXT("BOTH") ) == 0 ) ) {
        wrapperData->ignoreSignals = WRAPPER_IGNORE_SIGNALS_WRAPPER + WRAPPER_IGNORE_SIGNALS_JAVA;
    } else if ( strcmpIgnoreCase( val, TEXT("WRAPPER") ) == 0 ) {
        wrapperData->ignoreSignals = WRAPPER_IGNORE_SIGNALS_WRAPPER;
    } else if ( strcmpIgnoreCase( val, TEXT("JAVA") ) == 0 ) {
        wrapperData->ignoreSignals = WRAPPER_IGNORE_SIGNALS_JAVA;
    } else {
        wrapperData->ignoreSignals = 0;
    }

    /* Obtain the Console Title. */
    _sntprintf(propName, 256, TEXT("wrapper.console.title.%s"), wrapperOS);
    updateStringValue(&wrapperData->consoleTitle, getStringProperty(properties, propName, getStringProperty(properties, TEXT("wrapper.console.title"), NULL)));

    if (!wrapperData->configured) {
        /* Load the service name (Used to be windows specific so use those properties if set.) */
        updateStringValue(&wrapperData->serviceName, getStringProperty(properties, TEXT("wrapper.name"), getStringProperty(properties, TEXT("wrapper.ntservice.name"), TEXT("wrapper"))));

        /* Load the service display name (Used to be windows specific so use those properties if set.) */
        updateStringValue(&wrapperData->serviceDisplayName, getStringProperty(properties, TEXT("wrapper.displayname"), getStringProperty(properties, TEXT("wrapper.ntservice.displayname"), wrapperData->serviceName)));

        /* Load the service description, default to display name (Used to be windows specific so use those properties if set.) */
        updateStringValue(&wrapperData->serviceDescription, getStringProperty(properties, TEXT("wrapper.description"), getStringProperty(properties, TEXT("wrapper.ntservice.description"), wrapperData->serviceDisplayName)));

        /* Pausable */
        wrapperData->pausable = getBooleanProperty(properties, TEXT("wrapper.pausable"), getBooleanProperty(properties, TEXT("wrapper.ntservice.pausable"), FALSE));
        wrapperData->pausableStopJVM = getBooleanProperty(properties, TEXT("wrapper.pausable.stop_jvm"), getBooleanProperty(properties, TEXT("wrapper.ntservice.pausable.stop_jvm"), TRUE));
        wrapperData->initiallyPaused = getBooleanProperty(properties, TEXT("wrapper.pause_on_startup"), FALSE);
    }

#ifdef WIN32
    wrapperData->ignoreUserLogoffs = getBooleanProperty(properties, TEXT("wrapper.ignore_user_logoffs"), FALSE);

    /* Configure the NT service information */
    if (wrapperBuildNTServiceInfo()) {
        return TRUE;
    }

    if (wrapperData->generateConsole) {
        if (!wrapperData->ntAllocConsole) {
            /* We need to allocate a console in order for the thread dumps to work
             *  when running as a service.  But the user did not request that a
             *  console be visible. */
            wrapperData->ntAllocConsole = TRUE;
            wrapperData->ntShowWrapperConsole = FALSE; /* Unchanged actually */
        }
    }
#else /* UNIX */
    /* Configure the Unix daemon information */
    wrapperBuildUnixDaemonInfo();

    if (loadResourcesLimitsConfiguration()) {
        return TRUE;
    }
#endif

#ifdef WIN32
    if (!wrapperProcessHasVisibleConsole()) {
#else
    if (!wrapperData->isConsole) {
#endif
        /* The console is not visible, so we shouldn't waste time logging to it. */
        setConsoleLogLevelInt(LEVEL_NONE);
    }

    /* stdin */
#ifdef WIN32
    if (getConsoleInputMode() == WRAPPER_CONSOLE_INPUT_DISABLED) {
        wrapperData->disableConsoleInput = TRUE;
    } else {
        wrapperData->disableConsoleInput = FALSE;
    }
#else
    /* Not documented */
    wrapperData->javaNewProcessGroup = getBooleanProperty(properties, TEXT("wrapper.java.new_process_group"), TRUE);

    if (wrapperData->disableConsoleInputPermanent && wrapperData->javaNewProcessGroup) {
        /* Broken thread and no way to handle stdin. Force disabling. */
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Defective %s thread. %s"), TEXT("JavaIN"), TEXT("Disabling the ability to read stdin."));
        wrapperData->disableConsoleInput = TRUE;
    } else {
        switch (getConsoleInputMode()) {
        case WRAPPER_CONSOLE_INPUT_DISABLED:
            wrapperData->disableConsoleInput = TRUE;
            break;

        case WRAPPER_CONSOLE_INPUT_ENABLED:
            wrapperData->disableConsoleInput = FALSE;
            break;

        default:
            if (wrapperData->daemonize) {
                /* Daemonized, so no stdin */
                wrapperData->disableConsoleInput = TRUE;
            } else {
                /* Check if the file descriptor of stdin refers to a terminal. */
                if (isatty(STDIN_FILENO)) {
                    /* Get the terminal foreground process group. */
                    terminalPgid = tcgetpgrp(STDIN_FILENO);
                    if (terminalPgid == -1) {
                        /* If RUN_AS_USER is set in the Shell script, the process will be launched with 'runuser - <USER> -c "..."'.
                         *  In that case isatty() == 1 but tcgetpgrp() returns -1 with ENOTTY.  This is because the calling process
                         *  (runuser) creates a Login Shell.  Stdin of the Wrapper process refers to the Login Shell, but it differs
                         *  from the file descriptor of runuser.  runuser handles the redirection of the input stream, so this case
                         *  is ok.  We may need to refine this condition if we find other cases where it's better to disable stdin. */
                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("The file descriptor of stdin does not refer to the controlling terminal of the calling process. (0x%x) (Normal)"), errno);
                        wrapperData->disableConsoleInput = FALSE;
                    } else if (terminalPgid != getpgrp()) {
                        /* stdin refers to the terminal of the calling process, but the terminal is in a different process group.
                         *  This is the case when the Wrapper process was detached from the foreground process group,
                         *  e.g: './wrapper -c ../conf/wrapper.conf &'. */
                        if (kill(-terminalPgid, 0)) {
                            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("No terminal in the foreground process group.\n %s"), TEXT("Disabling the ability to read stdin."));
                        } else {
                            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("The process group the of the controlling terminal differs from the group of the Wrapper process.\n %s"), TEXT("Disabling the ability to read stdin."));
                        }
                        wrapperData->disableConsoleInput = TRUE;
                    } else {
                        /* The foreground process group of the terminal is the same as the Wrapper process. */
                        wrapperData->disableConsoleInput = FALSE;
                    }
                } else {
                    /* Not a tty. Stat the file to know what it is. */
                    fstat(STDIN_FILENO, &sb1);
                    
                    if (S_ISCHR(sb1.st_mode)) {
                        /* Character device.  Check if this is the null device. */
                        stat("/dev/null", &sb2);
                        if (sb1.st_rdev == sb2.st_rdev) {
                            /* Null device.  Case when Docker is run without tty. */
                            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Stdin is the null device. %s"), TEXT("Disabling the ability to read stdin."));
                            wrapperData->disableConsoleInput = FALSE;
                        } else {
                            /* Other character devices can be read.  Example: /dev/random */
                            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Stdin is a character device."));
                            wrapperData->disableConsoleInput = FALSE;
                        }
                    } else if (S_ISFIFO(sb1.st_mode)) {
                        /* e.g. 'echo myfile | ./wrapper -c ../conf/wrapper.conf' */
                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Stdin is a pipe."));
                        wrapperData->disableConsoleInput = FALSE;
                    } else if (S_ISREG(sb1.st_mode)) {
                        /* e.g. './wrapper -c ../conf/wrapper.conf < myfile' */
                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Stdin is a regular file."));
                        wrapperData->disableConsoleInput = FALSE;
                    } else if (S_ISSOCK(sb1.st_mode)) {
                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Stdin is a socket."));
                        wrapperData->disableConsoleInput = FALSE;
                    } else {
                        /* Other type: block device, dir, etc.  Should never happen.  No reason to enable. */
                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Stdin is of an unknown type (%u). %s"), sb1.st_mode, TEXT("Disabling the ability to read stdin."));
                        wrapperData->disableConsoleInput = TRUE;
                    }
                }
            }
        }
    }

    wrapperData->javaINFlush = getBooleanProperty(properties, TEXT("wrapper.restart.flush_input"), TRUE);
    if (!wrapperData->configured) {
        /* Changing the buffer size on reload is not allowed as it would complicate things if we don't flush. */
        wrapperData->javaINBufferSize = getIntProperty(properties, TEXT("wrapper.javain.buffer_size"), 8192);
        if (wrapperData->javaINBufferSize < WRAPPER_JAVAIO_BUFFER_SIZE_MIN) {
            wrapperData->javaINBufferSize = WRAPPER_JAVAIO_BUFFER_SIZE_MIN;
            log_printf(WRAPPER_SOURCE_WRAPPER, properties->logWarningLogLevel,
                TEXT("%s must be in the range %d to %d.  Changing to %d."), TEXT("wrapper.javain.buffer_size"), WRAPPER_JAVAIO_BUFFER_SIZE_MIN, WRAPPER_JAVAIO_BUFFER_SIZE_MAX, wrapperData->javaINBufferSize);
        } else if (wrapperData->javaINBufferSize > WRAPPER_JAVAIO_BUFFER_SIZE_MAX) {
            wrapperData->javaINBufferSize = WRAPPER_JAVAIO_BUFFER_SIZE_MAX;
            log_printf(WRAPPER_SOURCE_WRAPPER, properties->logWarningLogLevel,
                TEXT("%s must be in the range %d to %d.  Changing to %d."), TEXT("wrapper.javain.buffer_size"), WRAPPER_JAVAIO_BUFFER_SIZE_MIN, WRAPPER_JAVAIO_BUFFER_SIZE_MAX, wrapperData->javaINBufferSize);
        }
    }
#endif

#ifdef WIN32
    if (!wrapperData->configured) {
        /* Load privilege configuration, only one time on startup:
         *  - removing a privilege from the token of the current process is not reversible
         *  - once a privilege's status has been updated, trying to reset it to the same value would cause warnings */
        if (!wrapperData->isConsole) {
            /* When running as a service, the SCM will automatically add the SERVICE group to the service's primary token.
             *  This the case for most service accounts, including custom local accounts.
             *  The SERVICE group has the SeImpersonatePrivilege enabled by default. For local accounts, this can be
             *  dangerous because exploits being able to execute arbitrary code could abuse this privilege to escalate to
             *  the LocalSystem account which has even more privileges than administrators.
             *  SeImpersonatePrivilege is not needed for most Java applications, so we chose to disable it by default for safety. */
            val = getStringProperty(properties, TEXT("wrapper.ntservice.impersonation"), TEXT("DEFAULT"));
            if (strcmpIgnoreCase(val, TEXT("UNCHANGED")) == 0) {
                /* Never remove SeImpersonatePrivilege */
            } else if (strcmpIgnoreCase(val, TEXT("REMOVE")) == 0) {
                /* Always remove SeImpersonatePrivilege */
                wrapperData->removeImpersonation = TRUE;
            } else if (strcmpIgnoreCase(val, TEXT("DEFAULT")) == 0) {
                if (isLocalSystemAccount(wrapperData->ntServiceAccount)) {
                    /* Running with the LocalSystem account, so privileges cannot be escalated any higher. Leave SeImpersonatePrivilege as is. */
                } else if (currentServiceHasRequiredPrivileges()) {
                    /* When required privileges have been set, either SeImpersonatePrivilege
                     *  was not specified and it will be removed by the SCM, or it is wanted. */
                } else {
                    wrapperData->removeImpersonation = TRUE;
                }
            }
        }

        if (wrapperData->removeImpersonation) {
            /* Remove SeImpersonatePrivilege. */
            removeSeImpersonatePrivilege();
        }
    }
#endif

    wrapperData->configured = TRUE;

    return FALSE;
}

/**
 * Requests a lock on the tick mutex.
 *
 * @return TRUE if there were any problems, FALSE if successful.
 */
int wrapperLockTickMutex() {
#ifdef WIN32
    switch (WaitForSingleObject(tickMutexHandle, INFINITE)) {
    case WAIT_ABANDONED:
        _tprintf(TEXT("Tick was abandoned.\n"));
        return TRUE;
    case WAIT_FAILED:
        _tprintf(TEXT("Tick wait failed.\n"));
        return TRUE;
    case WAIT_TIMEOUT:
        _tprintf(TEXT("Tick wait timed out.\n"));
        return TRUE;
    default:
        /* Ok */
        break;
    }
#else
    if (pthread_mutex_lock(&tickMutex)) {
        _tprintf(TEXT("Failed to lock the Tick mutex. %s\n"), getLastErrorText());
        return TRUE;
    }
#endif

    return FALSE;
}

/**
 * Releases a lock on the tick mutex.
 *
 * @return TRUE if there were any problems, FALSE if successful.
 */
int wrapperReleaseTickMutex() {
#ifdef WIN32
    if (!ReleaseMutex(tickMutexHandle)) {
        _tprintf(TEXT("Failed to release tick mutex. %s\n"), getLastErrorText());
        return TRUE;
    }
#else
    if (pthread_mutex_unlock(&tickMutex)) {
        _tprintf(TEXT("Failed to unlock the tick mutex. %s\n"), getLastErrorText());
        return TRUE;
    }
#endif
    return FALSE;
}

/**
 * Calculates a tick count using the system time.
 *
 * We normally need 64 bits to do this calculation.  Play some games to get
 *  the correct values with 32 bit variables.
 */
TICKS wrapperGetSystemTicks() {
    static int firstCall = TRUE;
    static TICKS initialTicks = 0;
    struct timeb timeBuffer;
    DWORD high, low;
    TICKS sum;
#ifdef _DEBUG
    TICKS assertSum;
#endif

    wrapperGetCurrentTime(&timeBuffer);

    /* Break in half. */
    high = (DWORD)(timeBuffer.time >> 16) & 0xffff;
    low = (DWORD)(timeBuffer.time & 0xffff);

    /* Work on each half. */
    high = high * 1000 / WRAPPER_TICK_MS;
    low = (low * 1000 + timeBuffer.millitm) / WRAPPER_TICK_MS;

    /* Now combine them in such a way that the correct bits are truncated. */
    high = high + ((low >> 16) & 0xffff);
    sum = (TICKS)(((high & 0xffff) << 16) + (low & 0xffff));

    /* Check the result. */
#ifdef _DEBUG
#ifdef WIN32
    assertSum = (TICKS)((timeBuffer.time * 1000UI64 + timeBuffer.millitm) / WRAPPER_TICK_MS);
#else
    /* This will produce the following warning on some compilers:
     *  warning: ANSI C forbids long long integer constants
     * Is there another way to do this? */
    assertSum = (TICKS)((timeBuffer.time * 1000ULL + timeBuffer.millitm) / WRAPPER_TICK_MS);
#endif
    if (assertSum != sum) {
        _tprintf(TEXT("wrapperGetSystemTicks() resulted in %08x rather than %08x\n"), sum, assertSum);
    }
#endif

    if (firstCall) {
        initialTicks = sum - WRAPPER_TICK_INITIAL;
        firstCall = FALSE;
    }

    return (sum - initialTicks);
}

/**
 * Returns difference in seconds between the start and end ticks.  This function
 *  handles cases where the tick counter has wrapped between when the start
 *  and end tick counts were taken.  See the wrapperGetTicks() function.
 *
 * This can be done safely in 32 bits
 */
int wrapperGetTickAgeSeconds(TICKS start, TICKS end) {
    /*
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("      wrapperGetTickAgeSeconds(%08x, %08x) -> %08x"), start, end, (int)((end - start) * WRAPPER_TICK_MS) / 1000);
    */

    /* Simply subtracting the values will always work even if end has wrapped due to overflow.
     *  This is only true if end has wrapped by less than half of the range of TICKS/WRAPPER_TICK_MS!
     *  0x00000001 - 0xffffffff = 0x00000002 = 2
     *  0xffffffff - 0x00000001 = 0xfffffffe = -2
     */
    return (int)((end - start) * WRAPPER_TICK_MS) / 1000;
}

/**
 * Returns difference in ticks between the start and end ticks.  This function
 *  handles cases where the tick counter has wrapped between when the start
 *  and end tick counts were taken.  See the wrapperGetTicks() function.
 *
 * This can be done safely in 32 bits
 */
int wrapperGetTickAgeTicks(TICKS start, TICKS end) {
    /*
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("      wrapperGetTickAgeSeconds(%08x, %08x) -> %08x"), start, end, (int)(end - start));
    */

    /* Simply subtracting the values will always work even if end has wrapped due to overflow.
     *  This is only true if end has wrapped by less than half of the range of TICKS/WRAPPER_TICK_MS!
     *  0x00000001 - 0xffffffff = 0x00000002 = 2
     *  0xffffffff - 0x00000001 = 0xfffffffe = -2
     */
    return (int)(end - start);
}

/**
 * Returns TRUE if the specified tick timeout has expired relative to the
 *  specified tick count.
 */
int wrapperTickExpired(TICKS nowTicks, TICKS timeoutTicks) {
    /* Convert to a signed value. */
    int age = nowTicks - timeoutTicks;

    if (age >= 0) {
        return TRUE;
    } else {
        return FALSE;
    }
}

/**
 * Returns a tick count that is the specified number of seconds later than
 *  the base tick count.
 *
 * This calculation will work as long as the number of seconds is not large
 *  enough to require more than 32 bits when multiplied by 1000.
 */
TICKS wrapperAddToTicks(TICKS start, int seconds) {
    /*
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("      wrapperAddToTicks(%08x, %08x) -> %08x"), start, seconds, start + (seconds * 1000 / WRAPPER_TICK_MS));
    */
    return start + (seconds * 1000 / WRAPPER_TICK_MS);
}

/**
 * Do some sanity checks on the tick timer math.
 */
int wrapperTickAssertions() {
    int result = FALSE;
    TICKS ticks1, ticks2, ticksR, ticksE;
    int value1, valueR, valueE;

    /** wrapperGetTickAgeTicks test. */
    ticks1 = 0xfffffffe;
    ticks2 = 0xffffffff;
    valueE = 1;
    valueR = wrapperGetTickAgeTicks(ticks1, ticks2);
    if (valueR != valueE) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Assert Failed: wrapperGetTickAgeTicks(%08x, %08x) == %0d != %0d"), ticks1, ticks2, valueR, valueE);
        result = TRUE;
    }

    ticks1 = 0xffffffff;
    ticks2 = 0xfffffffe;
    valueE = -1;
    valueR = wrapperGetTickAgeTicks(ticks1, ticks2);
    if (valueR != valueE) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Assert Failed: wrapperGetTickAgeTicks(%08x, %08x) == %0d != %0d"), ticks1, ticks2, valueR, valueE);
        result = TRUE;
    }

    ticks1 = 0xffffffff;
    ticks2 = 0x00000000;
    valueE = 1;
    valueR = wrapperGetTickAgeTicks(ticks1, ticks2);
    if (valueR != valueE) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Assert Failed: wrapperGetTickAgeTicks(%08x, %08x) == %0d != %0d"), ticks1, ticks2, valueR, valueE);
        result = TRUE;
    }

    ticks1 = 0x00000000;
    ticks2 = 0xffffffff;
    valueE = -1;
    valueR = wrapperGetTickAgeTicks(ticks1, ticks2);
    if (valueR != valueE) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Assert Failed: wrapperGetTickAgeTicks(%08x, %08x) == %0d != %0d"), ticks1, ticks2, valueR, valueE);
        result = TRUE;
    }

    /** wrapperGetTickAgeSeconds test. */
    ticks1 = 0xfffffff0;
    ticks2 = 0xffffffff;
    valueE = 1;
    valueR = wrapperGetTickAgeSeconds(ticks1, ticks2);
    if (valueR != valueE) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Assert Failed: wrapperGetTickAgeSeconds(%08x, %08x) == %0d != %0d"), ticks1, ticks2, valueR, valueE);
        result = TRUE;
    }

    ticks1 = 0xffffffff;
    ticks2 = 0x0000000f;
    valueE = 1;
    valueR = wrapperGetTickAgeSeconds(ticks1, ticks2);
    if (valueR != valueE) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Assert Failed: wrapperGetTickAgeSeconds(%08x, %08x) == %0d != %0d"), ticks1, ticks2, valueR, valueE);
        result = TRUE;
    }

    ticks1 = 0x0000000f;
    ticks2 = 0xffffffff;
    valueE = -1;
    valueR = wrapperGetTickAgeSeconds(ticks1, ticks2);
    if (valueR != valueE) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Assert Failed: wrapperGetTickAgeSeconds(%08x, %08x) == %0d != %0d"), ticks1, ticks2, valueR, valueE);
        result = TRUE;
    }


    /** wrapperTickExpired test. */
    ticks1 = 0xfffffffe;
    ticks2 = 0xffffffff;
    valueE = FALSE;
    valueR = wrapperTickExpired(ticks1, ticks2);
    if (valueR != valueE) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Assert Failed: wrapperTickExpired(%08x, %08x) == %0d != %0d"), ticks1, ticks2, valueR, valueE);
        result = TRUE;
    }

    ticks1 = 0xffffffff;
    ticks2 = 0xffffffff;
    valueE = TRUE;
    valueR = wrapperTickExpired(ticks1, ticks2);
    if (valueR != valueE) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Assert Failed: wrapperTickExpired(%08x, %08x) == %0d != %0d"), ticks1, ticks2, valueR, valueE);
        result = TRUE;
    }

    ticks1 = 0xffffffff;
    ticks2 = 0x00000001;
    valueE = FALSE;
    valueR = wrapperTickExpired(ticks1, ticks2);
    if (valueR != valueE) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Assert Failed: wrapperTickExpired(%08x, %08x) == %0d != %0d"), ticks1, ticks2, valueR, valueE);
        result = TRUE;
    }

    ticks1 = 0x00000001;
    ticks2 = 0xffffffff;
    valueE = TRUE;
    valueR = wrapperTickExpired(ticks1, ticks2);
    if (valueR != valueE) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Assert Failed: wrapperTickExpired(%08x, %08x) == %0d != %0d"), ticks1, ticks2, valueR, valueE);
        result = TRUE;
    }

    /** wrapperAddToTicks test. */
    ticks1 = 0xffffffff;
    value1 = 1;
    ticksE = 0x00000009;
    ticksR = wrapperAddToTicks(ticks1, value1);
    if (ticksR != ticksE) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Assert Failed: wrapperAddToTicks(%08x, %d) == %08x != %08x"), ticks1, value1, ticksR, ticksE);
        result = TRUE;
    }

    return result;
}

/**
 * Set the uptime in seconds (based on internal tick counter and is valid up to one year from startup).
 */
void wrapperSetUptime(TICKS nowTicks, int *pSecs) {
    int uptimeSeconds;

    if (!wrapperData->uptimeFlipped) {
        uptimeSeconds = wrapperGetTickAgeSeconds(WRAPPER_TICK_INITIAL, nowTicks);
        if (uptimeSeconds > WRAPPER_MAX_UPTIME_SECONDS) {
            wrapperData->uptimeFlipped = TRUE;
            setUptime(0, TRUE);
        } else {
            setUptime(uptimeSeconds, FALSE);
        }
        if (pSecs) {
            *pSecs = uptimeSeconds;
        }
    }
}

/**
 * Sets the working directory of the Wrapper to the specified directory.
 *  The directory can be relative or absolute.
 * If there are any problems then a non-zero value will be returned.
 *
 * @param dir Directory to change to.
 *
 * @return TRUE if the directory failed to be set, FALSE otherwise.
 */
int wrapperSetWorkingDir(const TCHAR* dir) {
    int showOutput = wrapperData->configured;

    if (_tchdir(dir)) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL,
            TEXT("Unable to set working directory to: %s (%s)"), dir, getLastErrorText());
        return TRUE;
    }

    /* This function is sometimes called before the configuration is loaded. */
#ifdef _DEBUG
    showOutput = TRUE;
#endif

    if (showOutput) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Working directory set to: %s"), dir);
    }

    /* Set a variable to the location of the binary. */
    setEnv(TEXT("WRAPPER_WORKING_DIR"), dir, ENV_SOURCE_APPLICATION);

    return FALSE;
}

/******************************************************************************
 * Protocol callback functions
 *****************************************************************************/
void wrapperLogSignaled(int logLevel, TCHAR *msg) {
    /* */
    if (wrapperData->isDebugging) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Got a log message from JVM: %s"), msg);
    }
    /* */

    log_printf(wrapperData->jvmRestarts, logLevel, msg);
}

/**
 * Return TRUE if the correct key was received but the Wrapper failed to respond, FALSE otherwise.
 */
int wrapperKeyRegistered(TCHAR *key) {
    /* Allow for a large integer + \0 */
    int ret = FALSE;
    TCHAR buffer[11];

    if (wrapperData->isDebugging) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Got key from JVM: %s"), key);
    }
    switch (wrapperData->jState) {
    case WRAPPER_JSTATE_LAUNCHING:
        /* We now know that the Java side wrapper code has started and
         *  registered with a key.  We still need to verify that it is
         *  the correct key however. */
        if (_tcscmp(key, wrapperData->key) == 0) {
            /* This is the correct key. */
            wrapperSetJavaState(WRAPPER_JSTATE_LAUNCHED, 0, -1);

            /* Send the low log level to the JVM so that it can control output via the log method. */
            _sntprintf(buffer, 11, TEXT("%d"), getLowLogLevel());
            ret = ret || wrapperProtocolFunction(WRAPPER_MSG_LOW_LOG_LEVEL, buffer);

            /* Send the log file name. */
            ret = ret || sendLogFileName();

            /* Send the Wrapper Properties. */
            ret = ret || sendProperties();

            /* Send the System Properties. */
            ret = ret || sendAppProperties();
            wrapperFreeAppPropertyArray();

            /* Send Application Parameters. */
            if (wrapperData->useBackendParameters) {
                ret = ret || sendAppParameters();
                wrapperFreeAppParameterArray();
            } else {
                ret = ret || wrapperProtocolFunction(WRAPPER_MSG_APP_PARAMETERS, TEXT(""));
            }

            /* All information sent, inform the WrapperManager class that we are ready to start. */
            ret = ret || wrapperProtocolFunction(WRAPPER_MSG_PRESTART, TEXT("prestart"));

            if (ret) {
                /* Make sure to close the backend (if not already done). */
                wrapperProtocolClose();
                return TRUE;
            }
        } else {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT("Received a connection request with an incorrect key.  Waiting for another connection."));

            /* Set a flag to allow the response. */
            wrapperData->wrongKeyPacketReceived = TRUE;

            /* This was the wrong key.  Send a response. */
            wrapperProtocolFunction(WRAPPER_MSG_BADKEY, TEXT("Incorrect key.  Connection rejected."));

            /* No longer allow to write in this state. */
            wrapperData->wrongKeyPacketReceived = FALSE;

            /* Close the current connection but return FALSE.  Assume that the real JVM
             *  is still out there trying to connect.  So don't change
             *  the state.  If for some reason, this was the correct
             *  JVM, but the key was wrong.  then this state will time
             *  out and recover. */
            wrapperProtocolClose();
        }
        break;

    case WRAPPER_JSTATE_STOP:
        /* We got a key registration.  This means that the JVM thinks it was
         *  being launched but the Wrapper is trying to stop.  This state
         *  will clean up correctly. */
        break;

    case WRAPPER_JSTATE_STOPPING:
        /* We got a key registration.  This means that the JVM thinks it was
         *  being launched but the Wrapper is trying to stop.  Now that the
         *  connection to the JVM has been opened, tell it to stop cleanly. */
        wrapperSetJavaState(WRAPPER_JSTATE_STOP, 0, -1);
        break;

    default:
        /* We got a key registration that we were not expecting.  Ignore it. */
        break;
    }

    return FALSE;
}

/**
 * Called when a ping is first determined to be slower than the wrapper.ping.alert.threshold.
 *  This will happen before it has actually been responded to.
 *  The 'jvm_ping_slow' event will be raised for each ping that was detected slow, so if the
 *  JVM is hung for a long period of time, it may be raised several times. This can be used
 *  for analytics and to take actions like warning before the ping timeout occurs.
 */
void wrapperPingSlow() {
}

/**
 * Called when a ping is responded to, but was slower than the wrapper.ping.alert.threshold.
 *
 * @param tickAge The number of seconds it took to respond.
 */
void wrapperPingRespondedSlow(int tickAge) {
    log_printf(WRAPPER_SOURCE_WRAPPER, wrapperData->pingAlertLogLevel, TEXT("Pinging the JVM took %d seconds to respond."), tickAge);
}

/**
 * Called when a ping response is received.
 *
 * @param pingSendTicks Time in ticks when the ping was originally sent.
 * @param queueWarnings TRUE if warnings about the queue should be logged, FALSE if the ping response did not contain a time.
 */
void wrapperPingResponded(TICKS pingSendTicks, int queueWarnings) {
    TICKS nowTicks;
    int tickAge;
    PPendingPing pendingPing;
    int pingSearchDone;
    
#ifdef DEBUG_PING_QUEUE
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("    PING QUEUE Ping Response (tick %08x)"), pingSendTicks);
#endif
    /* We want to purge the ping from the PendingPing list. */
    do {
        pendingPing = wrapperData->firstPendingPing;
        if (pendingPing != NULL) {
            tickAge = wrapperGetTickAgeTicks(pingSendTicks, pendingPing->sentTicks);
#ifdef DEBUG_PING_QUEUE
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("    PING QUEUE First Queued Ping (tick %08x, age %d)"), pendingPing->sentTicks, tickAge);
#endif
            if (tickAge > 0) { /* pendingPing->sentTicks > pingSendTicks */
                /* We received a ping response that is earlier than the one we were expecting.
                 *  If the pendingPingQueue has overflown then we will stop writing to it.  Don't log warning messages when in this state as they would be confusing and are expected.
                 *  Leave this one in the queue for later. */
                if (queueWarnings) {
                    if ((!wrapperData->pendingPingQueueOverflow) && (!wrapperData->pendingPingQueueOverflowEmptied)) {
                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT("Received an unexpected ping response, sent at tick %08x.  First expected ping was sent at tick %08x."), pingSendTicks, pendingPing->sentTicks);
                    } else {
#ifdef DEBUG_PING_QUEUE
                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("    PING QUEUE Silently skipping unexpected ping response. (tick %08x) (First tick %08x)"), pingSendTicks, pendingPing->sentTicks);
#endif
                    }
                }
                pendingPing = NULL;
                pingSearchDone = TRUE;
            } else {
                if (tickAge < 0) {
                    /* This PendingPing object was sent before the PING that we received.  This means that we somehow lost a ping. */
                    if (queueWarnings) {
                        if ((!wrapperData->pendingPingQueueOverflow) && (!wrapperData->pendingPingQueueOverflowEmptied)) {
                            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT("Lost a ping response, sent at tick %08x."), pendingPing->sentTicks);
                        } else {
#ifdef DEBUG_PING_QUEUE
                            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("    PING QUEUE Silently skipping lost ping response. (tick %08x)"), pendingPing->sentTicks);
#endif
                        }
                    }
                    pingSearchDone = FALSE;
                } else {
                    /* This PendingPing object is for this PING event. */
                    pingSearchDone = TRUE;
#ifdef DEBUG_PING_QUEUE
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("    PING QUEUE Expected Ping Response. (tick %08x)"), pendingPing->sentTicks);
#endif
                    
                    /* When the emptied flag is set, we know that we are recovering from an overflow.
                     *  That flag is reset on the first expected PendingPing found in the queue. */
                    if (wrapperData->pendingPingQueueOverflowEmptied) {
                        wrapperData->pendingPingQueueOverflowEmptied = FALSE;
#ifdef DEBUG_PING_QUEUE
                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("    PING QUEUE Emptied Set flag reset."));
#endif
                    }
                }
                
                /* Detach the PendingPing from the queue. */
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
                    if (wrapperData->pendingPingQueueOverflow) {
                        wrapperData->pendingPingQueueOverflowEmptied = TRUE;
                        wrapperData->pendingPingQueueOverflow = FALSE;
#ifdef DEBUG_PING_QUEUE
                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("    PING QUEUE Reset Overflow, Emptied Set.") );
#endif
                    }
                }
                
                /* Free up the pendingPing object. */
                if (pendingPing != NULL) {
                    free(pendingPing);
                    pendingPing = NULL;
                }
            }
        } else {
            /* Got a ping response when the queue was empty. */
            if (queueWarnings) {
                if ((!wrapperData->pendingPingQueueOverflow) && (!wrapperData->pendingPingQueueOverflowEmptied)) {
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT("Received an unexpected ping response, sent at tick %08x."), pingSendTicks);
                } else {
#ifdef DEBUG_PING_QUEUE
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("    PING QUEUE Silently skipping unexpected ping response. (tick %08x) (Empty)"), pingSendTicks);
#endif
                }
            }
            pingSearchDone = TRUE;
        }
    } while (!pingSearchDone);
    
    /* Depending on the current JVM state, do something. */
    switch (wrapperData->jState) {
    case WRAPPER_JSTATE_STARTED:
        /* We got a response to a ping. */
        nowTicks = wrapperGetTicks();
        
        /* Figure out how long it took for us to get this ping response in seconds. */
        tickAge = wrapperGetTickAgeSeconds(pingSendTicks, nowTicks);
        
        /* If we took longer than the threshold then we want to log a message. */
        if ((wrapperData->pingAlertThreshold > 0) && (tickAge >= wrapperData->pingAlertThreshold)) {
            wrapperPingRespondedSlow(tickAge);
        }
        
        /* Allow 5 + <pingTimeout> more seconds before the JVM is considered to be dead. */
        if (wrapperData->pingTimeout > 0) {
            wrapperUpdateJavaStateTimeout(nowTicks, 5 + wrapperData->pingTimeout);
        } else {
            wrapperUpdateJavaStateTimeout(nowTicks, -1);
        }

        break;

    default:
        /* We got a ping response that we were not expecting.  Ignore it. */
        break;
    }
}

void wrapperPingTimeoutResponded() {
    wrapperProcessActionList(wrapperData->pingActionList, TEXT("JVM appears hung: Timed out waiting for signal from JVM."),
                             WRAPPER_ACTION_SOURCE_CODE_PING_TIMEOUT, 0, TRUE, wrapperData->errorExitCode);
}

void wrapperStopRequested(int exitCode) {
    if (wrapperData->isDebugging) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG,
            TEXT("JVM requested a shutdown. (%d)"), exitCode);
    }

    /* Remember that a stop message was received so we don't try to send any other messages to the JVM. */
    wrapperData->stopPacketReceived = TRUE;

    /* Get things stopping on this end.  Ask the JVM to stop again in case the
     *  user code on the Java side is not written correctly. */
    wrapperStopProcess(exitCode, FALSE);
}

void wrapperRestartRequested() {
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("JVM requested a restart."));
    
    /* Make a note of the fact that we received this restart packet. */
    wrapperData->restartPacketReceived = TRUE;
    
    wrapperRestartProcess();
}

/**
 * If the current state of the JVM is STOPPING then this message is used to
 *  extend the time that the wrapper will wait for a STOPPED message before
 *  giving up on the JVM and killing it.
 */
void wrapperStopPendingSignaled(int waitHint) {
    if (wrapperData->isDebugging) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("JVM signaled a stop pending with waitHint of %d millis."), waitHint);
    }

    if (wrapperData->jState == WRAPPER_JSTATE_STARTED) {
        /* Change the state to STOPPING */
        wrapperSetJavaState(WRAPPER_JSTATE_STOPPING, 0, -1);
        /* Don't need to set the timeout here because it will be set below. */
    }

    if (wrapperData->jStateTimeoutTicksSet) {
        if (wrapperData->jState == WRAPPER_JSTATE_STOPPING) {
            if (waitHint < 0) {
                waitHint = 0;
            }

            wrapperUpdateJavaStateTimeout(wrapperGetTicks(), (int)ceil(waitHint / 1000.0));
        }
    }
}

/**
 * The wrapper received a signal from the JVM that it has completed the stop
 *  process.  If the state of the JVM is STOPPING, then change the state to
 *  STOPPED.  It is possible to get this request after the Wrapper has given up
 *  waiting for the JVM.  In this case, the message is ignored.
 */
void wrapperStoppedSignaled() {
    if (wrapperData->isDebugging) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("JVM signaled that it was stopped."));
    }
    
    /* If the restart mode is already set and it is WRAPPER_RESTART_REQUESTED_AUTOMATIC but we
     *  have not yet received a RESTART packet, this means that state engine got confused because
     *  of an unexpected delay.  The fact that the STOPPED packet arived but not the RESTART packet
     *  means that the application did not intend for the restart to take place.
     * Reset the restart and let the Wrapper exit. */
    if ((wrapperData->restartRequested == WRAPPER_RESTART_REQUESTED_AUTOMATIC) && (!wrapperData->restartPacketReceived)) {
        /* If we get here it is because the Wrapper previously decided to do a restart to recover.
         *  That means that another message was already shown to the user.  We want to show another
         *  message here so there is a record of why we don't restart. */
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("Received Stopped packet late.  Cancel automatic restart."));
        
        wrapperData->restartRequested = WRAPPER_RESTART_REQUESTED_NO;
    }
    
    /* Make a note of the fact that we received this stopped packet. */
    wrapperData->stoppedPacketReceived = TRUE;

    if ((wrapperData->jState == WRAPPER_JSTATE_LAUNCHED) ||
        (wrapperData->jState == WRAPPER_JSTATE_STARTING) ||
        (wrapperData->jState == WRAPPER_JSTATE_STARTED) ||
        (wrapperData->jState == WRAPPER_JSTATE_STOPPING)) {
        /* The Java side of the wrapper signaled that it stopped
         *  allow 5 + jvmExitTimeout seconds for the JVM to exit. */
        if (wrapperData->jvmExitTimeout > 0) {
            wrapperSetJavaState(WRAPPER_JSTATE_STOPPED, wrapperGetTicks(), 5 + wrapperData->jvmExitTimeout);
        } else {
            wrapperSetJavaState(WRAPPER_JSTATE_STOPPED, 0, -1);
        }
    }
}

/**
 * If the current state of the JVM is STARTING then this message is used to
 *  extend the time that the wrapper will wait for a STARTED message before
 *  giving up on the JVM and killing it.
 */
void wrapperStartPendingSignaled(int waitHint) {
    if (wrapperData->isDebugging) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("JVM signaled a start pending with waitHint of %d millis."), waitHint);
    }

    /* Only process the start pending signal if the JVM state is starting or
     *  stopping.  Stopping are included because if the user hits CTRL-C while
     *  the application is starting, then the stop request will not be noticed
     *  until the application has completed its startup. */
    if (wrapperData->jStateTimeoutTicksSet) {
        if ((wrapperData->jState == WRAPPER_JSTATE_STARTING) ||
            (wrapperData->jState == WRAPPER_JSTATE_STOPPING)) {
            if (waitHint < 0) {
                waitHint = 0;
            }

            wrapperUpdateJavaStateTimeout(wrapperGetTicks(), (int)ceil(waitHint / 1000.0));
        }
    }
}

/**
 * The wrapper received a signal from the JVM that it has completed the startup
 *  process.  If the state of the JVM is STARTING, then change the state to
 *  STARTED.  It is possible to get this request after the Wrapper has given up
 *  waiting for the JVM.  In this case, the message is ignored.
 */
void wrapperStartedSignaled() {
    if (wrapperData->isDebugging) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("JVM signaled that it was started."));
    }


    if (wrapperData->jState == WRAPPER_JSTATE_STARTING) {
        /* We got the expected started packed.  Now start pinging.  Allow 5 + <pingTimeout> more seconds before the JVM
         *  is considered to be dead. */
        if (wrapperData->pingTimeout > 0) {
            wrapperSetJavaState(WRAPPER_JSTATE_STARTED, wrapperGetTicks(), 5 + wrapperData->pingTimeout);
        } else {
            wrapperSetJavaState(WRAPPER_JSTATE_STARTED, 0, -1);
        }
        /* Is the wrapper state STARTING? */
        if (wrapperData->wState == WRAPPER_WSTATE_STARTING) {
            wrapperSetWrapperState(WRAPPER_WSTATE_STARTED);

            if (!wrapperData->isConsole) {
                /* Tell the service manager that we started */
                wrapperReportStatus(FALSE, WRAPPER_WSTATE_STARTED, 0, 0);
            }
        }
        
        /* If we are configured to detach and shutdown when the JVM is started then start doing so. */
        if (wrapperData->detachStarted) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("JVM launched and detached.  Wrapper Shutting down..."));
            wrapperProtocolClose();
            wrapperDetachJava();
            wrapperStopProcess(0, FALSE);
        }
        
    } else if (wrapperData->jState == WRAPPER_JSTATE_STOP) {
        /* This will happen if the Wrapper was asked to stop as the JVM is being launched. */
    } else if (wrapperData->jState == WRAPPER_JSTATE_STOPPING) {
        /* This will happen if the Wrapper was asked to stop as the JVM is being launched. */
        wrapperSetJavaState(WRAPPER_JSTATE_STOP, 0, -1);
    }
}

/**
 * Get the parent process id
 *  ATTENTION: in some rare cases CreateToolhelp32Snapshot can have infinite cycles when looping on the parent processes
 *  It is the responsibility of the caller not to fall into an infinite loop.
 *
 * @param pid process id
 * @param found TRUE if the parent process could be found.
 *
 * @return the parent process id
 */
#ifdef WIN32
DWORD wrapperGetPPid(DWORD pid, int *found) {
    DWORD ppid = 0;
    HANDLE h = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 pe = { 0 };
    
    pe.dwSize = sizeof(PROCESSENTRY32);
    if (found != NULL) {
        *found = FALSE;
    }
    
    if( Process32First(h, &pe)) {
        do {
            if (pe.th32ProcessID == pid) {
                ppid = pe.th32ParentProcessID;
                if (found != NULL) {
                    *found = TRUE;
                }
                break;
            }
        } while( Process32Next(h, &pe));
    }

    CloseHandle(h);
    return ppid;
}
#endif

#ifdef WIN32
 #ifdef MAX_WAIT_PS
  #define MAX_WAIT_CHECKPPID  MAX_WAIT_PS + 100    /* ms */
 #else
  #define MAX_WAIT_CHECKPPID  2000                 /* ms */
 #endif

/**
 * Check ancestry relationship between two processes.
 *  NOTE: this function will give up if a matching parent can't be found after 100 iterations.
 *  It keeps a list of all parents found in order to prevent infinite loop (see comment in wrapperGetPPid)
 *
 * @param pid   Id of the child process.
 * @param ppid  Id to be searched among pid's ancestors.
 * @param depth 0 if the research should include the pid itself,
 *              1 if it should start from its direct parent,
 *              2 from its grand-parent,
 *                etc.
 * @param pError pointer that will be set TRUE if the function could not complete because of an error.
 *
 * @return TRUE if ppid is found among pid's ancestors.
 */
int wrapperCheckPPid(DWORD pid, DWORD ppid, int depth, int* pError) {
    DWORD pids[100] = {0};
    int i = 0;
    int j = 0;
    int found = FALSE;
    TICKS start = wrapperGetTicks();
    
    do {
 #ifdef DEBUG_PPID
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_INFO, TEXT("  pid:%d - ppid:%d"), pid, ppid);
 #endif
        if ((i >= depth) && (pid == ppid)) {
            return TRUE;
        }
        
        /* add pid to the list of the PIDs found */
        pids[j] = pid;
        
        pid = wrapperGetPPid(pid, &found);
        if (!found) {
            if (pError) {
                *pError = TRUE;
            }
            return FALSE;
        }
        
        /* make sure that the parent PID was not already found before */
        j = 0;
        while (pids[j] != 0) {
            if (pids[j] == pid) {
                /* circular ancestry */
                if (pError) {
                    *pError = TRUE;
                }
                return FALSE;
            }
            j++;
        }
        
        /* Make sure this function doesn't take too long */
        if (wrapperGetTickAgeTicks(start, wrapperGetTicks()) * WRAPPER_TICK_MS > MAX_WAIT_CHECKPPID) {
            if (pError) {
                *pError = TRUE;
            }
            return FALSE;
        }
    } while ((pid != 0) && (i++ < 100));
    
    return FALSE;
}
#endif

/**
 * Check if the PID file should be blocking.
 *
 * return 1 if the file already exists and we are strict, 0 otherwise.
 */
int checkPidFile() {
    if (wrapperData->pidFileStrict && wrapperData->pidFilename && wrapperFileExists(wrapperData->pidFilename)) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR,
            TEXT("%d pid file, %s, already exists."),
            wrapperData->wrapperPID, wrapperData->pidFilename);
        /* We should not clean up files of the other instance running. */
        return 1;
    }
    return 0;
}

/**
 * Write the Wrapper PID file, anchor file and lockfile when the Wrapper starts up.
 *
 * return 1 if there is any error, 0 otherwise.
 */
int wrapperWriteStartupPidFiles() {
    if (wrapperData->pidFilename) {
        if (writePidFile(wrapperData->pidFilename, wrapperData->wrapperPID, wrapperData->pidFileUmask
#ifndef WIN32
            , wrapperData->pidFileGroup
#endif
            )) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL,
                TEXT("ERROR: Could not write pid file %s: %s"),
                wrapperData->pidFilename, getLastErrorText());
            return 1;
        }
    }

    if (wrapperData->anchorFilename) {
        if (writePidFile(wrapperData->anchorFilename, wrapperData->wrapperPID, wrapperData->anchorFileUmask
#ifndef WIN32
            , wrapperData->anchorFileGroup
#endif
            )) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL,
                TEXT("ERROR: Could not write anchor file %s: %s"),
                wrapperData->anchorFilename, getLastErrorText());
            return 1;
        }
    }

    if (wrapperData->lockFilename) {
        if (writePidFile(wrapperData->lockFilename, wrapperData->wrapperPID, wrapperData->lockFileUmask
#ifndef WIN32
            , wrapperData->lockFileGroup
#endif
            )) {
            /* This will fail if the user is running as a user without full privileges.
             *  To make things easier for user configuration, this is ignored if sufficient
             *  privileges do not exist. */
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS,
                TEXT("WARNING: Could not write lock file %s: %s"),
                wrapperData->lockFilename, getLastErrorText());
            wrapperData->lockFilename = NULL;
        }
    }
    return 0;
}

#ifdef CUNIT
static void tsJAP_subTestJavaAdditionalParamSuite(int quotable, TCHAR *config, TCHAR **strings, int strings_len, int isJVMParam) {
    LoadParameterFileCallbackParam param;
    int ret;
    /*int i;*/

    param.quotable = quotable;
    param.strings = NULL;
    param.index = 0;
    param.isJVMParam = isJVMParam;
    param.isAppProperty = FALSE;
    param.hasCipher = FALSE;
    param.hasInvalidArg = FALSE;

    ret = loadParameterFileCallback((void *)(&param), NULL, 0, 0, config, TRUE, LEVEL_DEBUG);
    CU_ASSERT_TRUE(ret);
    if (!ret) {
        return;
    }
    CU_ASSERT(strings_len == param.index);

    param.strings = (TCHAR **)malloc(sizeof(TCHAR *) * strings_len);
    if (!param.strings) {
        return;
    }
    
    param.index = 0;
    param.isJVMParam = isJVMParam;

    ret = loadParameterFileCallback((void *)(&param), NULL, 0, 0, config, TRUE, LEVEL_DEBUG);
    CU_ASSERT_TRUE(ret);
    if (!ret) {
        return;
    }
    CU_ASSERT(strings_len == param.index);

    if (!param.strings) {
        return;
    }
    
    /*for (i = 0; i < strings_len; i++) {
        CU_ASSERT(_tcscmp(strings[i], param.strings[i]) == 0);
    }*/

    /*for (i = 0; i < strings_len; i++) {
        free(param.strings[i]);
    }*/
    free(param.strings);
}

#define TSJAP_ARRAY_LENGTH(a) (sizeof(a) / sizeof(a[0]))

void tsJAP_testJavaAdditionalParamSuite(void) {
    int quotable;
    int i = 0;
    int isJVM = TRUE;
    for (i = 0; i < 2; i++) {
        _tprintf(TEXT("%d round\n"), i);
        if (i > 0) {
            isJVM = FALSE;
        }
        /* Test set #1 */
        {
            /* Single parameter in 1 line. */
            TCHAR *config = TEXT("-Dsomething=something");
            TCHAR *strings[1];
            strings[0] = TEXT("-Dsomething=something");
            tsJAP_subTestJavaAdditionalParamSuite(FALSE, config, strings, TSJAP_ARRAY_LENGTH(strings), isJVM);
        }
        {
            /* Multiple parameters in 1 line. */
            TCHAR *config = TEXT("-Dsomething=something -Dxxx=xxx");
            TCHAR *strings[2];
            strings[0] = TEXT("-Dsomething=something");
            strings[1] = TEXT("-Dxxx=xxx");
            tsJAP_subTestJavaAdditionalParamSuite(FALSE, config, strings, TSJAP_ARRAY_LENGTH(strings), isJVM);
        }
        {
            /* Horizontal Tab is not a delimiter. */
            TCHAR *config = TEXT("-Dsomething1=something1\t-Dsomething2=something2 -Dxxx=xxx");
            TCHAR *strings[2];
            strings[0] = TEXT("-Dsomething1=something1\t-Dsomething2=something2");
            strings[1] = TEXT("-Dxxx=xxx");
            tsJAP_subTestJavaAdditionalParamSuite(FALSE, config, strings, TSJAP_ARRAY_LENGTH(strings), isJVM);
        }
        {
            /* Horizontal Tab is not a delimiter. */
            TCHAR *config = TEXT("-Dsomething1=something1\t-Dsomething2=something2 -Dxxx=xxx");
            TCHAR *strings[2];
            strings[0] = TEXT("-Dsomething1=something1\t-Dsomething2=something2");
            strings[1] = TEXT("-Dxxx=xxx");
            tsJAP_subTestJavaAdditionalParamSuite(FALSE, config, strings, TSJAP_ARRAY_LENGTH(strings), isJVM);
        }
        if (isJVM) {
            {
                /* A parameter without heading '-' will be skipped. */
                TCHAR *config = TEXT("something=something -Dxxx=xxx");
                TCHAR *strings[1];
                strings[0] = TEXT("-Dxxx=xxx");
                tsJAP_subTestJavaAdditionalParamSuite(FALSE, config, strings, TSJAP_ARRAY_LENGTH(strings), isJVM);
            }
        } else {
            {
            /* A parameter without heading '-' will not be skipped. */
            TCHAR *config = TEXT("something=something -Dxxx=xxx");
            TCHAR *strings[2];
            strings[0] = TEXT("something=something");
            strings[1] = TEXT("-Dxxx=xxx");
            tsJAP_subTestJavaAdditionalParamSuite(FALSE, config, strings, TSJAP_ARRAY_LENGTH(strings), isJVM);
            }
        }

        /* Test set #2 : without stripping double quotations */
        quotable = FALSE;    
        {
            /* Quotations #1 */
            TCHAR *config = TEXT("-DmyApp.x1=\"Hello World.\" -DmyApp.x2=x2");
            TCHAR *strings[2];
            strings[0] = TEXT("-DmyApp.x1=\"Hello World.\"");
            strings[1] = TEXT("-DmyApp.x2=x2");
            tsJAP_subTestJavaAdditionalParamSuite(quotable, config, strings, TSJAP_ARRAY_LENGTH(strings), isJVM);
        }
        {
            /* Quotations #2 */
            TCHAR *config = TEXT("\"-DmyApp.x1=Hello World.\" -DmyApp.x2=x2");
            TCHAR *strings[2];
            strings[0] = TEXT("\"-DmyApp.x1=Hello World.\"");
            strings[1] = TEXT("-DmyApp.x2=x2");
            tsJAP_subTestJavaAdditionalParamSuite(quotable, config, strings, TSJAP_ARRAY_LENGTH(strings), isJVM);
        }
        {
            /* Escaped quotation */
            TCHAR *config = TEXT("-DmyApp.x1=\"Hello \\\"World.\" -DmyApp.x2=x2");
            TCHAR *strings[2];
            strings[0] = TEXT("-DmyApp.x1=\"Hello \\\"World.\"");
            strings[1] = TEXT("-DmyApp.x2=x2");
            tsJAP_subTestJavaAdditionalParamSuite(quotable, config, strings, TSJAP_ARRAY_LENGTH(strings), isJVM);
        }
        {
            /* Escaped backslash */
            TCHAR *config = TEXT("-DmyApp.x1=\"Hello World.\\\\\" -DmyApp.x2=x2");
            TCHAR *strings[2];
            strings[0] = TEXT("-DmyApp.x1=\"Hello World.\\\\\"");
            strings[1] = TEXT("-DmyApp.x2=x2");
            tsJAP_subTestJavaAdditionalParamSuite(quotable, config, strings, TSJAP_ARRAY_LENGTH(strings), isJVM);
        }
        /* Test set #3 : with stripping double quotations */
        quotable = TRUE;
        {
            /* Quotations #1 */
            TCHAR *config = TEXT("-DmyApp.x1=\"Hello World.\" -DmyApp.x2=x2");
            TCHAR *strings[2];
            strings[0] = TEXT("-DmyApp.x1=Hello World.");
            strings[1] = TEXT("-DmyApp.x2=x2");
            tsJAP_subTestJavaAdditionalParamSuite(quotable, config, strings, TSJAP_ARRAY_LENGTH(strings), isJVM);
        }
        {
            /* Quotations #2 */
            TCHAR *config = TEXT("\"-DmyApp.x1=Hello World.\" -DmyApp.x2=x2");
            TCHAR *strings[2];
            strings[0] = TEXT("-DmyApp.x1=Hello World.");
            strings[1] = TEXT("-DmyApp.x2=x2");
            tsJAP_subTestJavaAdditionalParamSuite(quotable, config, strings, TSJAP_ARRAY_LENGTH(strings), isJVM);
        }
        {
            /* Escaped quotation */
            TCHAR *config = TEXT("-DmyApp.x1=\"Hello \\\"World.\" -DmyApp.x2=x2");
            TCHAR *strings[2];
            strings[0] = TEXT("-DmyApp.x1=Hello \"World.");
            strings[1] = TEXT("-DmyApp.x2=x2");  
            tsJAP_subTestJavaAdditionalParamSuite(quotable, config, strings, TSJAP_ARRAY_LENGTH(strings), isJVM);
        }
        {
            /* Escaped backslash */
            TCHAR *config = TEXT("-DmyApp.x1=\"Hello World.\\\\\" -DmyApp.x2=x2");
            TCHAR *strings[2];
            strings[0] = TEXT("-DmyApp.x1=Hello World.\\");
            strings[1] = TEXT("-DmyApp.x2=x2");
            tsJAP_subTestJavaAdditionalParamSuite(quotable, config, strings, TSJAP_ARRAY_LENGTH(strings), isJVM);
        }
    }
}
#endif /* CUNIT */
