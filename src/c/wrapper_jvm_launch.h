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

#ifndef _WRAPPER_JVM_LAUNCH_H
 #define _WRAPPER_JVM_LAUNCH_H

 #include "wrapper.h"

 #define JAVA_PROC_COMPLETED      0   /* java process executed successfully */
 #define JAVA_PROC_LAUNCH_FAILED  1   /* failed to launch */
 #define JAVA_PROC_WAIT_FAILED    2   /* launched but failed to wait for the process (timed out or another error) */
 #define JAVA_PROC_KILL_FAILED    3   /* failed to wait, and then failed to kill the process */
 #define JAVA_PROC_INTERRUPTED    4   /* interrupted by a signal */

/**
 * Callback to be called upon completion of a Java query.
 *
 * @param nowTicks The current tick.
 * @param queryResult One of the JAVA_PROC_* constants returned by wrapperQueryJava().
 * @param queryDesc The description of the query.
 *
 * @return -1 if the query resulted in a critical failure, or was interrupted,
 *          1 if the query failed but the Wrapper can continue,
 *          0 if the query completed successfully.
 */
typedef int (*QueryCallback)(TICKS nowTicks, int queryResult, const TCHAR* queryDesc);

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
int wrapperLaunchJavaVersion(QueryCallback callback, TICKS nowTicks);

/**
 * Create a child process to find the jar file containing the main class.
 *
 * @param callback Callback to call before returning 
 * @param nowTicks The current tick
 *
 * @return the value returned by the callback, or 0 if not defined.
 */
int wrapperLaunchBootstrap(QueryCallback callback, TICKS nowTicks);

/**
 * Launch the Java command with '--dry-run' to check if it is valid.
 *
 * @param callback Callback to call before returning 
 * @param nowTicks The current tick
 *
 * @return the value returned by the callback, or 0 if not defined.
 */
int wrapperLaunchDryJavaApp(QueryCallback callback, TICKS nowTicks);

#endif
