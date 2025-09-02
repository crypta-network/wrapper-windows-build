/*
 * Copyright (c) 1999, 2025 Tanuki Software, Ltd.
 * http://www.tanukisoftware.com
 * All rights reserved.
 *
 * This software is the proprietary information of Tanuki Software.
 * You shall use it only in accordance with the terms of the
 * license agreement you entered into with Tanuki Software.
 * http://wrapper.tanukisoftware.com/doc/english/licenseOverview.html
 */

/**
 * Author:
 *   Tanuki Software Development Team <support@tanukisoftware.com>
 */

#ifndef _WRAPPER_FILE_H
#define _WRAPPER_FILE_H

#ifdef WIN32
#include <tchar.h>
#endif
#include "property.h"
#include "wrapper_i18n.h"
#include "wrapper_hashmap.h"

/*#define WRAPPER_FILE_DEBUG*/

/**
 * Callback declaration which can be passed to calls to configFileReader.
 */
typedef int (*ConfigFileReader_Callback)(void *param, const TCHAR *fileName, int lineNumber, int depth, TCHAR *config, int expandVars, int minLogLevel);

/**
 * Callback declaration of a function to debug the configuration file.
 */
typedef void (*ConfigFileReader_DebugCallback)(void *callbackParam, int exitOnOverwrite, int logLevelOnOverwrite);

/* Structure used by configFileReader to read files. */
typedef struct ConfigFileReader ConfigFileReader;
struct ConfigFileReader {
    ConfigFileReader_Callback callback;
    ConfigFileReader_ReadFilterCallbackMB readFilterCallback;
    ConfigFileReader_DebugCallback debugCallback;
    void *callbackParam;
    const TCHAR *paramFilePropName;
    int enableIncludes;
    int preload;
    int minLogLevel;
    int includeRequiredByDefault;       /* TRUE if the default mode for file inclusions is 'required', FALSE otherwise. */
    int debugIncludes;                  /* debugIncludes controls whether or not debug output is logged. It is set using directives in the file being read. */
    int logLevelOnOverwrite;
    int exitOnOverwrite;
};

/**
 * Tests whether a file exists.
 *
 * @return TRUE if exists, FALSE otherwise.
 */
extern int wrapperFileExists(const TCHAR * filename);

#ifdef WIN32
extern int wrapperGetUNCFilePath(const TCHAR *path, int advice);
#endif

extern const TCHAR* getFileName(const TCHAR* path);

#ifdef WRAPPER_FILE_DEBUG
extern void wrapperFileTests();
#endif

#define CONFIG_FILE_READER_SUCCESS      101
#define CONFIG_FILE_READER_FAIL         102
#define CONFIG_FILE_READER_OPEN_FAIL    103

/**
 * Read a configuration file.
 *
 * @param filename Name of configuration file to read.
 * @param fileRequired TRUE if the file specified by filename is required, FALSE if a missing
 *                     file will silently fail.
 * @param callback Pointer to a callback function which will be called for each line read.
 * @param callbackParam Pointer to additional user data which will be passed to the callback.
 * @param readFilterCallback Pointer to a callback funtion which will be used to filter some
 *                           lines that should not be processed (optional).
 * @param debugCallback Pointer to a callback funtion which can be used for debugging, e.g.
 *                      to copy debug info to the callbackParam data (optional).
 * @param paramFilePropName Optional name of the property where the file to read is referenced (used
 *                     for parameter files).
 * @param enableIncludes If TRUE then includes will be supported.
 * @param preload TRUE if this is a preload pass.
 * @param minLogLevel log level above which messages should be logged.
 * @param originalWorkingDir Working directory of the binary at the moment it was launched.
 * @param warnedVarMap Map of undefined environment variables for which the user was warned.
 * @param ignoreVarMap Map of environment variables that should not be expanded.
 * @param logWarnings Flag that controls whether or not warnings will be logged.
 * @param logWarningLogLevel Log level at which any log warnings will be logged.
 *
 * @return CONFIG_FILE_READER_SUCCESS if the file was read successfully,
 *         CONFIG_FILE_READER_OPEN_FAIL if the file could not be found or opened,
 *         CONFIG_FILE_READER_FAIL if an error has occurred and loading should stop.
 */
extern int configFileReader(const TCHAR *filename,
                            int fileRequired,
                            ConfigFileReader_Callback callback,
                            void *callbackParam,
                            ConfigFileReader_ReadFilterCallbackMB readFilterCallback,
                            ConfigFileReader_DebugCallback debugCallback,
                            const TCHAR* paramFilePropName,
                            int enableIncludes,
                            int preload,
                            int minLogLevel,
                            const TCHAR *originalWorkingDir,
                            PHashMap warnedVarMap,
                            PHashMap ignoreVarMap,
                            int logWarnings,
                            int logWarningLogLevel);

#endif

