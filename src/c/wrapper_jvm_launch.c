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
 *   Tanuki Software Development Team <support@tanukisoftware.com>
 */

#include "logger.h"
#include "wrapper.h"
#include "wrapper_jvm_launch.h"

/**
 * Create a child process to print the Java version running the command:
 *    /path/to/java -version
 *  After printing the java version, the process is terminated.
 * 
 * In case the JVM is slow to start, it will time out after
 * the number of seconds set in "wrapper.java.version.timeout".
 *
 * Note: before the timeout is reached, the user can ctrl+c to stop the Wrapper.
 *
 * @param callback Callback to call before returning 
 * @param nowTicks The current tick
 *
 * @return the value returned by the callback, or 0 if not defined.
 */
int wrapperLaunchJavaVersion(QueryCallback callback, TICKS nowTicks) {
    const TCHAR* desc = TEXT("Java Version");
    int exitCode = 0;
    int result;

    if (wrapperData->printJVMVersion) {
        wrapperData->jvmDefaultLogLevel = __max(wrapperData->javaQueryLogLevel, LEVEL_INFO);
    } else {
        wrapperData->jvmDefaultLogLevel = wrapperData->javaQueryLogLevel;
    }
    wrapperData->jvmCallType = WRAPPER_JVM_VERSION;

    setJvmQrySource(TEXT("jvm ver."));

    initJavaVersionParser();

    log_printf(WRAPPER_SOURCE_WRAPPER, wrapperData->javaQueryLogLevel, TEXT("Java Command Line (%s):"), desc);
    printJavaCommand(wrapperData->jvmVersionCommand, wrapperData->javaQueryLogLevel, FALSE);

    /* If the user sets the value to 0, then we will wait indefinitely. */
    result = wrapperQueryJava(wrapperData->jvmVersionCommand, desc, TRUE, wrapperData->javaVersionTimeout, TRUE, &exitCode);

    switch (result) {
    case JAVA_PROC_COMPLETED:
        if (exitCode == 0) {
            if (!javaVersionFound()) {
                wrapperSetJavaVersion(NULL);
            }
        } else {
            /* Resolve the Java version to its default value. */
            wrapperSetJavaVersion(NULL);
        }
        break;

    case JAVA_PROC_WAIT_FAILED:
        /* Reset wrapperData->javaVersion. */
        if (wrapperData->javaVersion) {
            disposeJavaVersion(wrapperData->javaVersion);
            wrapperData->javaVersion = NULL;
        }
        
        /* There might be no output but read all the pipe anyway. */
        while (wrapperReadChildOutput(250)) { }
        
        /* Continue only if, by chance, the Java version was found. */
        if (wrapperData->javaVersion) {
            result = JAVA_PROC_COMPLETED;
        }
        break;

    default:
        break;
    }

    wrapperData->javaQueryPID = 0;

    /* Some errors may have been queued when parsing the Java output. Print them now. */
    maintainLogger();

    return callback ? callback(nowTicks, result, desc) : 0;
}

/**
 * Create a child process to find the jar file containing the main class.
 *
 * @param callback Callback to call before returning 
 * @param nowTicks The current tick
 *
 * @return the value returned by the callback, or 0 if not defined.
 */
int wrapperLaunchBootstrap(QueryCallback callback, TICKS nowTicks) {
    const TCHAR* desc = TEXT("Bootstrap");
    int exitCode = 0;
    int result;

    wrapperData->jvmDefaultLogLevel = wrapperData->javaQueryLogLevel;
    wrapperData->jvmCallType = WRAPPER_JVM_BOOTSTRAP;

    setJvmQrySource(TEXT("jvm btsp"));

    initJavaBootstrapParser();

    log_printf(WRAPPER_SOURCE_WRAPPER, wrapperData->javaQueryLogLevel, TEXT("Java Command Line (%s):"), desc);
    printJavaCommand(wrapperData->jvmBootstrapCommand, wrapperData->javaQueryLogLevel, FALSE);

    result = wrapperQueryJava(wrapperData->jvmBootstrapCommand, desc, TRUE, wrapperData->javaQueryTimeout, FALSE, &exitCode);

    wrapperData->javaQueryPID = 0;

    maintainLogger();

    if (result == JAVA_PROC_COMPLETED) {
        if (exitCode == 0) {
            if (!wrapperData->jvmBootstrapVersionOk) {
                if (wrapperData->jvmBootstrapVersionMismatch) {
                    /* Error already reported. */
                } else {
                    /* We now know that the process is complete, but there was no version in the output. */
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Incorrect Java bootstrap output.  Please verify that this is a valid Wrapper jar file."));
                }
                wrapperData->jvmBootstrapFailed = TRUE;
            }
        } else {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Java bootstrap step failed (exit code: %d)."), exitCode);
            if (!wrapperData->wrapperJar) {
                /* wrapper.jarfile was not specified. It's likely that this is the cause of the error, but it's also possible that wrapper.jar exists somewhere in the classpath.
                 *  It is difficult to check the classpath as the file can be renamed. wrapper.jar may also exist in the CLASSPATH environment.
                 *  => Anyway advise to use wrapper.jarfile. */
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("  Missing Wrapper jar file?  Please use the %s property."), TEXT("wrapper.jarfile"));
            }
            wrapperData->jvmBootstrapFailed = TRUE;
        }
    }

    return callback ? callback(nowTicks, result, desc) : 0;
}

/**
 * Launch the Java command with '--dry-run' to check if it is valid.
 *
 * @param callback Callback to call before returning 
 * @param nowTicks The current tick
 *
 * @return the value returned by the callback, or 0 if not defined.
 */
int wrapperLaunchDryJavaApp(QueryCallback callback, TICKS nowTicks) {
    const TCHAR* desc = TEXT("Dry Run");
    int exitCode = 0;
    int result;
#ifdef WIN32
    JAVA_COMMAND_TYPE dryCmd = wrapperData->jvmDryCommandPrint;
    JAVA_COMMAND_TYPE cmd = wrapperData->jvmCommandPrint;
#else
    JAVA_COMMAND_TYPE dryCmd = wrapperData->jvmDryCommand;
    JAVA_COMMAND_TYPE cmd = wrapperData->jvmCommand;
#endif

    wrapperData->jvmDefaultLogLevel = wrapperData->javaQueryLogLevel;
    wrapperData->jvmCallType = WRAPPER_JVM_DRY;

    setJvmQrySource(TEXT("jvm dry "));

    log_printf(WRAPPER_SOURCE_WRAPPER, wrapperData->javaQueryLogLevel, TEXT("Java Command Line (%s):"), desc);
    printJavaCommand(dryCmd, wrapperData->javaQueryLogLevel, FALSE);

    result = wrapperQueryJava(wrapperData->jvmDryCommand, desc, FALSE, wrapperData->javaQueryTimeout, FALSE, &exitCode);

    wrapperData->javaQueryPID = 0;

    if ((result == JAVA_PROC_COMPLETED) && (exitCode != 0)) {
        /* Note: A misspelling of the main class should have been caught during the bootstrap step, so the remaining possible errors are the use of wrong Java options.*/
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("--------------------------------------------------------------------"));
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("The Java command line is invalid."));
        printJavaCommand(cmd, wrapperData->commandLogLevel, !wrapperData->jvmCommandShowBackendProps); /* print at the loglevel chosen by the user (by default DEBUG to not disclose information contained in the command line) */
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT(""));
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Usually this is caused by an option that does not exist or is not\n  valid for the Java version being used (java version \"%d.%d.%d\")."), wrapperData->javaVersion->major, wrapperData->javaVersion->minor, wrapperData->javaVersion->revision);
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Please check the values of the wrapper.java.additional.<n>\n  properties against the above JVM output."));
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("--------------------------------------------------------------------"));

        wrapperData->jvmDryRunFailed = TRUE;
    }

    return callback ? callback(nowTicks, result, desc) : 0;
}
