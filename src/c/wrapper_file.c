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

#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#ifdef WIN32
 #include <errno.h>
 #include <tchar.h>
 #include <io.h>
 #include <winsock.h>
#else
 #include <stdlib.h>
 #include <string.h>
 #include <unistd.h>
 #include <langinfo.h>
 #include <limits.h>
#endif

#include "wrapper_file.h"
#include "logger.h"

#ifndef TRUE
#define TRUE 1
#endif

#ifndef FALSE
#define FALSE 0
#endif

#define MAX_INCLUDE_DEPTH 10

/**
 * Tests whether a file exists.
 *
 * @return TRUE if exists, FALSE otherwise.
 */
int wrapperFileExists(const TCHAR * filename) {
    FILE * file;
    if ((file = _tfopen(filename, TEXT("r")))) {
        fclose(file);
        return TRUE;
    }
    return FALSE;
}

#ifdef WIN32
/**
 * @param path to check.
 * @param advice 0 if advice should be displayed.
 *
 * @return advice or advice + 1 if advice was logged.
 */
int wrapperGetUNCFilePath(const TCHAR *path, int advice) {
    TCHAR drive[4];
    DWORD result;

    /* See if the path starts with a drive.  Some users use forward slashes in the paths. */
    if ((path != NULL) && (_tcslen(path) >= 3) && (path[1] == TEXT(':')) && ((path[2] == TEXT('\\')) || (path[2] == TEXT('/')))) {
        _tcsncpy(drive, path, 2);
        drive[2] = TEXT('\\');
        drive[3] = TEXT('\0');
        result = GetDriveType(drive);
        if (result == DRIVE_REMOTE) {
            if (advice == 0) {
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ADVICE, TEXT("The following path in your Wrapper configuration file is to a mapped Network\n  Drive.  Using mapped network drives is not recommended as they will fail to\n  be resolved correctly under certain circumstances.  Please consider using\n  UNC paths (\\\\<host>\\<share>\\path). Additional references will be ignored.\n  Path: %s"), path);
                advice++;
            }
        } else if (result == DRIVE_NO_ROOT_DIR) {
            if (advice == 0) {
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ADVICE, TEXT("The following path in your Wrapper configuration file could not be resolved.\n  Please make sure the path exists.  If the path is a network share, it may be\n  that the current user is unable to resolve it.  Please consider using UNC\n  paths (\\\\<host>\\<share>\\path) or run the service as another user\n  (see wrapper.ntservice.account). Additional references will be ignored.\n  Path: %s"), path);
                advice++;
            }
        }
    }
    return advice;
}
#endif

/**
 * This function searches for the last directory separator in the given string
 *  and returns a pointer to its location + 1, or simply returns the string if
 *  no separator is found. It does no malloc.
 *
 * NOTE: special cases to be supported:
 *       - path/filename.ext\" => filename.ext\" (the final quote should be kept)
 *       - filename.ext\" => filename.ext\"
 */
const TCHAR* getFileName(const TCHAR* path) {
    const TCHAR* ptr1;
#ifdef WIN32
    const TCHAR* ptr2;
#endif
    
    if (path) {
        ptr1 = _tcsrchr(path, TEXT('/'));
#ifdef WIN32
        ptr2 = _tcsrchr(path, TEXT('\\'));
        if (!ptr1 && !ptr2) {
            return path;
        } else if (!ptr1) {
            return ptr2 + 1;
        } else if (!ptr2) {
            return ptr1 + 1;
        } else if (&ptr2 > &ptr1) {
            return ptr2 + 1;
#else
        if (!ptr1) {
            return path;
#endif
        } else {
            return ptr1 + 1;
        }
    }
    return NULL;
}

#define CONFIG_LINE_READ_SUCCESS  0
#define CONFIG_LINE_READ_COMPLETE 1
#define CONFIG_LINE_READ_SKIP     2
#define CONFIG_LINE_READ_FAIL     3

/**
 * Read a line of configuration file.
 *
 * @param bufferMB Multibytes buffer to fill
 * @param stream   the file to read
 * @param reader   the reader
 * @param pLineNum pointer to the current line number
 *                 (will be incremented if the line is read successfully)
 *
 * @return CONFIG_LINE_READ_SUCCESS
 *         CONFIG_LINE_READ_COMPLETE
 *         CONFIG_LINE_READ_SKIP
 *         CONFIG_LINE_READ_FAIL
 */
int readConfigLine(char *bufferMB, FILE *stream, ConfigFileReader *reader, int* pLineNum) {
    int result;

    if (fgets(bufferMB, MAX_PROPERTY_NAME_VALUE_LENGTH, stream)) {
        /* Does this line contain sensitive data? */
        if (reader->readFilterCallback && !reader->readFilterCallback(bufferMB)) {
#ifdef _DEBUG
            printf("configFileReader_Read - address of bufferMB[0] (%c): %p\n", bufferMB[0], &(bufferMB[0]));
            printf("configFileReader_Read - address of bufferMB[1] (%c): %p\n", bufferMB[1], &(bufferMB[1]));
#endif
            result = CONFIG_LINE_READ_SKIP;
        } else {
            result = CONFIG_LINE_READ_SUCCESS;
        }
        (*pLineNum)++;
    } else if (feof(stream)) {
        result = CONFIG_LINE_READ_COMPLETE;
    } else {
        result = CONFIG_LINE_READ_FAIL;
    }
    return result;
}

/**
 * Read configuration file.
 */
int configFileReader_Read(ConfigFileReader *reader,
                          const TCHAR *filename,
                          int fileRequired,
                          int depth,
                          const TCHAR *parentFilename,
                          int parentLineNumber,
                          const TCHAR *originalWorkingDir,
                          PHashMap warnedVarMap,
                          PHashMap ignoreVarMap,
                          int logWarnings,
                          int logWarningLogLevel) {
    FILE *stream;
    char bufferMB[MAX_PROPERTY_NAME_VALUE_LENGTH];
    int readLineRet;
    TCHAR expBuffer[MAX_PROPERTY_NAME_VALUE_LENGTH];
    TCHAR *trimmedBuffer;
    size_t trimmedBufferLen;
    TCHAR directiveChar;
    TCHAR *c;
    TCHAR *d;
    size_t i, j;
    size_t len;
    int quoted;
    TCHAR *absoluteBuffer;
    int hasBOM;
    int hasUserEncoding;
    int lineNumber = 0;
    int includeRequiredByDefault = reader->includeRequiredByDefault;
    int debugIncludes            = reader->debugIncludes;
    int exitOnOverwrite          = reader->exitOnOverwrite;
    int logLevelOnOverwrite      = reader->logLevelOnOverwrite;
    int onOverwriteChanged;
    char *encodingMB;
#ifdef WIN32
    int encoding;
#else
    char* encoding;
    char* interumEncoding;
    TCHAR* encodingW = NULL;
    TCHAR* interumEncodingW = NULL;
    size_t size;
#endif
    int includeRequired;
    int readResult = CONFIG_FILE_READER_SUCCESS;
    int ret;
    TCHAR *bufferW;
    size_t bufferWSize;
#ifdef WIN32
    int size;
#endif
    int expandVars = TRUE; /* only applies to the current file, not to include files! */
    int delegateParsing;
#ifdef _DEBUG
    TCHAR *logLevelNames[] = { TEXT("NONE  "), TEXT("DEBUG "), TEXT("INFO  "), TEXT("STATUS"), TEXT("WARN  "), TEXT("ERROR "), TEXT("FATAL "), TEXT("ADVICE"), TEXT("NOTICE") };

    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS, TEXT("configFileReader_Read('%s', required %d, depth %d, parent '%s', number %d, debugIncludes %d, preload %d, minLogLevel %d)"),
        filename, fileRequired, depth, (parentFilename ? parentFilename : TEXT("<NULL>")), parentLineNumber, reader->debugIncludes, reader->preload, reader->minLogLevel);
#endif

    /* Look for the specified file. */
    if ((stream = _tfopen(filename, TEXT("rb"))) == NULL) {
        /* Unable to open the file. */
        if (reader->minLogLevel <= (fileRequired ? LEVEL_FATAL : LEVEL_STATUS)) {
            if (reader->debugIncludes || fileRequired) {
                if (reader->paramFilePropName) {
                    log_printf(WRAPPER_SOURCE_WRAPPER, fileRequired ? LEVEL_FATAL : LEVEL_WARN,
                        TEXT("File referenced in property %s not found: %s\n  Current working directory: %s"), reader->paramFilePropName, filename, originalWorkingDir);
                } else if (depth > 0) {
                    log_printf(WRAPPER_SOURCE_WRAPPER, fileRequired ? LEVEL_FATAL : LEVEL_WARN,
                        TEXT("%sIncluded configuration file not found: %s\n  Referenced from: %s (line %d)\n  Current working directory: %s"),
                        (reader->debugIncludes ? TEXT("  ") : TEXT("")), filename, parentFilename, parentLineNumber, originalWorkingDir);
                } else {
                    log_printf(WRAPPER_SOURCE_WRAPPER, fileRequired ? LEVEL_FATAL : LEVEL_STATUS,
                        TEXT("Configuration file not found: %s\n  Current working directory: %s"), filename, originalWorkingDir);
                }
            } else {
#ifdef _DEBUG
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Configuration file not found: %s"), filename);
#endif
            }
        }
        return CONFIG_FILE_READER_OPEN_FAIL;
    }

    if (reader->debugIncludes) {
        if (reader->minLogLevel <= LEVEL_STATUS) {
            if (depth > 0) {
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS,
                    TEXT("  Reading included configuration file, %s"), filename);
            } else {
                /* Will not actually get here because the debug includes can't be set until it is loaded.
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS,
                    TEXT("Reading configuration file, %s"), filename); */
            }
        }
    }

    /* Load in the first row of configurations to check the encoding. */
    readLineRet = readConfigLine(bufferMB, stream, reader, &lineNumber);
    if (readLineRet == CONFIG_LINE_READ_FAIL) {
        /* Failed to read the first line of the file. */
        if (reader->minLogLevel <= LEVEL_ERROR) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR, TEXT("%sError reading configuration file, %s."), TEXT("  "), filename);
        }

        /* The stream can't be read, so we can't go any further. Nothing in this file has been loaded yet, so the file can be skipped if optional. Simply fail. */
        fclose(stream);
        return CONFIG_FILE_READER_FAIL;
    } else if (readLineRet == CONFIG_LINE_READ_COMPLETE) {
        /* Empty file. */
        fclose(stream);
        return CONFIG_FILE_READER_SUCCESS;
    } else {
        /* If the file starts with a BOM (Byte Order Marker) then we want to skip over it. */
        if ((bufferMB[0] == (char)0xef) && (bufferMB[1] == (char)0xbb) && (bufferMB[2] == (char)0xbf)) {
            i = 3;
            hasBOM = TRUE;
        } else {
            i = 0;
            hasBOM = FALSE;
        }

        if (readLineRet == CONFIG_LINE_READ_SKIP) { /* continue with this return code so that the line will be skipped in the loop below. */
            /* Fill the buffer with 0 to ensure nothing remains in memory. */
            wrapperSecureZero(bufferMB, sizeof(bufferMB));

            hasUserEncoding = FALSE;
        } else {
            /* Does the file start with "@encoding="? (or "#encoding=" for compatibility) */
            if ((strlen(bufferMB) >= (i + 10)) &&
               ((bufferMB[i++] == '@') || (bufferMB[i - 1] == '#')) && (bufferMB[i++] == 'e') && (bufferMB[i++] == 'n') &&
                (bufferMB[i++] == 'c') && (bufferMB[i++] == 'o') && (bufferMB[i++] == 'd') && (bufferMB[i++] == 'i') &&
                (bufferMB[i++] == 'n') && (bufferMB[i++] == 'g') && (bufferMB[i++] == '=')) {
                encodingMB = bufferMB + i;
                i = 0;
                while ((encodingMB[i] != ' ') && (encodingMB[i] != '\n') && (encodingMB[i]  != '\r')) {
                   i++;
                }
                encodingMB[i] = '\0';

                if ((hasBOM) && (strIgnoreCaseCmp(encodingMB, "UTF-8") != 0)) {
                    if (reader->minLogLevel <= LEVEL_WARN) {
                        if (reader->paramFilePropName) {
                            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN,
                                TEXT("The encoding type of the parameter file, %s, is not specified as 'UTF-8' but the file has a BOM marker meaning that it is encoded as 'UTF-8'."), filename);
                        } else {
                            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN,
                                TEXT("The encoding type of the configuration file, %s, is not specified as 'UTF-8' but the file has a BOM marker meaning that it is encoded as 'UTF-8'."), filename);
                        }
                    }
                }
                if (getEncodingByName(encodingMB, &encoding)) {
                    if (reader->minLogLevel <= LEVEL_ERROR) {
#ifdef WIN32
                        if (reader->paramFilePropName) {
                            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR,
                                TEXT("The specified encoding type '% S' in parameter file, %s, is not currently supported. Please use 'UTF-8'."),
                                encodingMB, filename);
                        } else {
                            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR,
                                TEXT("%sThe specified encoding type '% S' in configuration file, %s, is not currently supported. Please use 'UTF-8'."),
                                (reader->debugIncludes ? TEXT("  ") : TEXT("")), encodingMB, filename);
                        }
#else
                        if (reader->paramFilePropName) {
                            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR,
                                TEXT("The specified encoding type '% s' in parameter file, %s, is not currently supported. Please use 'UTF-8'."),
                                encodingMB, filename);
                        } else {
                            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR,
                                TEXT("%sThe specified encoding type '% s' in configuration file, %s, is not currently supported. Please use 'UTF-8'."),
                                (reader->debugIncludes ? TEXT("  ") : TEXT("")), encodingMB, filename);
                        }
#endif
                    }
                    fclose(stream);
                    return CONFIG_FILE_READER_FAIL;
                }

                /* Read next line */
                readLineRet = readConfigLine(bufferMB, stream, reader, &lineNumber);

                hasUserEncoding = TRUE;
            } else {
                hasUserEncoding = FALSE;
            }
        }

        if (!hasUserEncoding) {
            if (hasBOM) {
                /* The file was marked with a UTF-8 BOM. */
                getEncodingByName("UTF-8", &encoding);

                if (reader->minLogLevel < LEVEL_ERROR) {
                    if (reader->paramFilePropName) {
                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG,
                            TEXT("An encoding declaration is missing from the top of the parameter file, %s, but a BOM marker was found. Using UTF-8."), filename);
                    } else if (reader->debugIncludes) {
                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS,
                            TEXT("%sAn encoding declaration is missing from the top of the configuration file, %s, but a BOM marker was found. Using UTF-8."), TEXT("  "), filename);
                    } else {
                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG,
                            TEXT("%sAn encoding declaration is missing from the top of the configuration file, %s, but a BOM marker was found. Using UTF-8."), TEXT(""), filename);
                    }
                }

                if (readLineRet != CONFIG_LINE_READ_SKIP) {
                    /* bufferMB will be used to read the line in the loop below. We need to skip the BOM (3 characters). */
                    for (i = 3; i < strlen(bufferMB); i++) {
                        bufferMB[i - 3] = bufferMB[i];
                    }
                    bufferMB[i - 3] = 0;
                }
            } else {
#ifdef WIN32
                encoding = GetACP();
#else 
                encoding = nl_langinfo(CODESET);
 #ifdef MACOSX
                if (strlen(encoding) == 0) {
                    encoding = "UTF-8";
                }
 #endif
#endif
                if (reader->minLogLevel < LEVEL_ERROR) {
                    if (reader->paramFilePropName) {
                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG,
                            TEXT("An encoding declaration is missing from the top of the parameter file '%s'. Trying the system encoding."), filename);
                    } else if (reader->debugIncludes) {
                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS,
                            TEXT("%sAn encoding declaration is missing from the top of the configuration file '%s'. Trying the system encoding."), TEXT("  "), filename);
                    } else {
                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG,
                            TEXT("%sAn encoding declaration is missing from the top of the configuration file '%s'. Trying the system encoding."), TEXT(""), filename);
                    }
                }
            }
        }
    }

    if (depth == 0) {
        /* Before loading any configuration we need to reset the default 'on_overwrite' settings. */
        if (reader->debugCallback) {
            (*reader->debugCallback)(reader->callbackParam, reader->exitOnOverwrite, reader->logLevelOnOverwrite);
        }
    }

    /* Read all of the configurations */
    while (readLineRet != CONFIG_LINE_READ_COMPLETE) {
        /* First check if this line should be skipped (in some modes we know that passwords are not used, so we want to avoid store them in memory) */
        if (readLineRet == CONFIG_LINE_READ_SKIP) {
            /* Fill the buffer with 0 to ensure nothing remains in memory. */
            wrapperSecureZero(bufferMB, sizeof(bufferMB));

            readLineRet = readConfigLine(bufferMB, stream, reader, &lineNumber);

            continue;
        } else if (readLineRet == CONFIG_LINE_READ_FAIL) {
            fclose(stream);
            wrapperSecureZero(bufferMB, sizeof(bufferMB));
            return CONFIG_FILE_READER_FAIL;
        }

#ifdef WIN32
        ret = multiByteToWideChar(bufferMB, encoding, &bufferW, TRUE);
#else
        interumEncoding = nl_langinfo(CODESET);
 #ifdef MACOSX
        if (strlen(interumEncoding) == 0) {
            interumEncoding = "UTF-8";
        }
 #endif
        ret = multiByteToWideChar(bufferMB, encoding, interumEncoding, &bufferW, TRUE);
#endif
        if (ret) {
            if (bufferW) {
                /* bufferW contains an error message. */
                if (reader->minLogLevel <= LEVEL_ERROR) {
                    if (reader->paramFilePropName) {
                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR,
                            TEXT("Parameter file, %s, contains a problem on line #%d and could not be read. (%s)"), filename, lineNumber, bufferW);
                    } else if (depth > 0) {
                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR,
                            TEXT("%sIncluded configuration file, %s, contains a problem on line #%d and could not be read. (%s)"),
                            (reader->debugIncludes ? TEXT("  ") : TEXT("")), filename, lineNumber, bufferW);
                    } else {
                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR,
                            TEXT("Configuration file, %s, contains a problem on line #%d and could not be read. (%s)"), filename, lineNumber, bufferW);
                    }
#ifndef WIN32
                    /* On Windows there is no such problem because wide chars are always UTF-16 which should support most characters. */
                    size = mbstowcs(NULL, encoding, 0);
                    if (size > (size_t)0) {
                        encodingW = malloc(sizeof(TCHAR) * (size + 1));
                        if (encodingW) {
                            mbstowcs(encodingW, encoding, size + 1);
                        }
                    }
                    size = mbstowcs(NULL, interumEncoding, 0);
                    if (size > (size_t)0) {
                        interumEncodingW = malloc(sizeof(TCHAR) * (size + 1));
                        if (interumEncodingW) {
                            mbstowcs(interumEncodingW, interumEncoding, size + 1);
                        }
                    }
                    if (!compareEncodingsSysMode(encodingW, interumEncodingW)) {
                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ADVICE,
                            TEXT("One possible cause of failure is when the encoding of the locale used by the\n  Wrapper doesn't support certain characters of the configuration file.\n  The encoding of the current locale is '%s'. It is advised to set it\n  identical to the encoding of the configuration file (%s),\n  either by changing the default system locale or by using the wrapper.lang and\n  wrapper.lang.<platform>.encoding properties."), interumEncodingW, encodingW);
                    }
                    free(encodingW);
                    free(interumEncodingW);
#endif
                }
                free(bufferW);
            } else {
                outOfMemory(TEXT("RCF"), 1);
            }
            if (reader->preload) {
                /* On preload, ignore the line and continue. We want to load as much as possible and hopefully load the appropriate locale to read the file on the second load. */
                lineNumber++;
                continue;
            }
            fclose(stream);
            wrapperSecureZero(bufferMB, sizeof(bufferMB));
            return CONFIG_FILE_READER_FAIL;
        }
        /* Store the size of the buffer because the callback will insert a 0 after any valid property name. */
        bufferWSize = _tcslen(bufferW) * sizeof(TCHAR);
        
#ifdef _DEBUG
        /* The line feeds are not yet stripped here. */
        /*
 #ifdef WIN32
        wprintf(TEXT("%s:%d (%d): [%s]\n"), filename, lineNumber, encoding, bufferW);
 #else
        wprintf(TEXT("%S:%d (%s to %s): [%S]\n"), filename, lineNumber, encoding, interumEncoding, bufferW);
 #endif
        */
#endif
            
        c = bufferW;
        /* Always strip both ^M and ^J off the end of the line, this is done rather
         *  than simply checking for \n so that files will work on all platforms
         *  even if their line feeds are incorrect. */
        if ((d = _tcschr(bufferW, 0x0d /* ^M */)) != NULL) {
            d[0] = TEXT('\0');
        }
        if ((d = _tcschr(bufferW, 0x0a /* ^J */)) != NULL) {
            d[0] = TEXT('\0');
        }
        /* Strip any whitespace from the front of the line. */
        trimmedBuffer = bufferW;
        while ((trimmedBuffer[0] == TEXT(' ')) || (trimmedBuffer[0] == TCHAR_TAB)) {
            trimmedBuffer++;
        }

        /* If the line does not start with a comment, make sure that
         *  any comment at the end of line are stripped.  If at any point, a
         *  double hash, '##', is encountered it should be interpreted as a
         *  hash in the actual property rather than the beginning of a comment. */
        if (trimmedBuffer[0] != TEXT('#')) {
            len = _tcslen(trimmedBuffer);
            i = 0;
            quoted = 0;
            while (i < len) {
                if (trimmedBuffer[i] == TEXT('"')) {
                    quoted = !quoted;
                } else if ((trimmedBuffer[i] == TEXT('#')) && (!quoted)) {
                    /* Checking the next character will always be ok because it will be
                     *  '\0 at the end of the string. */
                    if (trimmedBuffer[i + 1] == TEXT('#')) {
                        /* We found an escaped #. Shift the rest of the string
                         *  down by one character to remove the second '#'.
                         *  Include the shifting of the '\0'. */
                        for (j = i + 1; j <= len; j++) {
                            trimmedBuffer[j - 1] = trimmedBuffer[j];
                        }
                        len--;
                    } else {
                        /* We found a comment. So this is the end. */
                        trimmedBuffer[i] = TEXT('\0');
                        len = i;
                    }
                }
                i++;
            }
        }

        /* Strip any whitespace from the end of the line. */
        trimmedBufferLen = _tcslen(trimmedBuffer);
        while ((trimmedBufferLen > 0) && ((trimmedBuffer[trimmedBufferLen - 1] == TEXT(' ')) || (trimmedBuffer[trimmedBufferLen - 1] == TCHAR_TAB))) {
            trimmedBuffer[--trimmedBufferLen] = TEXT('\0');
        }

        /* Only look at lines which contain data. */
        if (_tcslen(trimmedBuffer) > 0) {
            delegateParsing = FALSE;
            
            /* The line starts with '#' or '@', it may be a known directive (or a comment). */
            if ((trimmedBuffer[0] == TEXT('#')) || (trimmedBuffer[0] == TEXT('@'))) {
                directiveChar = trimmedBuffer[0];
                trimmedBuffer++;
                if (_tcsstr(trimmedBuffer, TEXT("include")) == trimmedBuffer) {
                    if (reader->enableIncludes) {
                        if (strcmpIgnoreCase(trimmedBuffer, TEXT("include.debug")) == 0) {
                            /* Enable include file debugging. */
                            if (!reader->preload) {
#ifdef _DEBUG
                                if (!reader->debugIncludes) {
                                    _tprintf(TEXT("debug include: FALSE -> TRUE\n  file: %s (%d)\n"), filename, lineNumber);
                                }
#endif
                                reader->debugIncludes = TRUE;
                                if ((depth == 0) && (reader->minLogLevel <= LEVEL_STATUS)) {
                                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS,
                                        TEXT("Base configuration file is %s"), filename);
                                }
                            } else {
                                reader->debugIncludes = FALSE;
                            }
                        } else if (_tcsstr(trimmedBuffer, TEXT("include.default_mode=")) == trimmedBuffer) {
                            trimmedBuffer += 21;
                            if (strcmpIgnoreCase(trimmedBuffer, TEXT("required")) == 0) {
#ifdef _DEBUG
                                if (!reader->preload && !reader->includeRequiredByDefault) {
                                    _tprintf(TEXT("include mode : optional -> required\n  file: %s (%d)\n"), filename, lineNumber);
                                }
#endif
                                reader->includeRequiredByDefault = TRUE;
                            } else if (strcmpIgnoreCase(trimmedBuffer, TEXT("optional")) == 0) {
#ifdef _DEBUG
                                if (!reader->preload && reader->includeRequiredByDefault) {
                                    _tprintf(TEXT("include mode : required -> optional\n  file: %s (%d)\n"), filename, lineNumber);
                                }
#endif
                                reader->includeRequiredByDefault = FALSE;
                            } else if (reader->minLogLevel <= LEVEL_WARN) {
                                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN,
                                    TEXT("Encountered an invalid value for directive %c%s=%s (line %d).  Ignoring this directive."),
                                    directiveChar, TEXT("include.default_mode"), trimmedBuffer, lineNumber);
                            }
                        } else if ((_tcsstr(trimmedBuffer, TEXT("include ")) == trimmedBuffer) ||
                                   (_tcsstr(trimmedBuffer, TEXT("include.optional ")) == trimmedBuffer) ||
                                   (_tcsstr(trimmedBuffer, TEXT("include.required ")) == trimmedBuffer)) {
                            if ((_tcsstr(trimmedBuffer, TEXT("include.required ")) == trimmedBuffer)) {
                                /* The include file is explicitly marked as required. */
                                includeRequired = TRUE;
                                c = trimmedBuffer + 17;
                            } else if ((_tcsstr(trimmedBuffer, TEXT("include.optional ")) == trimmedBuffer)) {
                                /* The include file is explicitly marked as optional. */
                                includeRequired = FALSE;
                                c = trimmedBuffer + 17;
                            } else {
                                /* Default include file (use the value of #include.default_mode to determine if it is required or not) */
                                includeRequired = reader->includeRequiredByDefault;
                                c = trimmedBuffer + 8;
                            }
                            
                            /* Strip any leading whitespace */
                            while ((c[0] != TEXT('\0')) && (c[0] == TEXT(' '))) {
                                c++;
                            }
                            
                            /* The filename may contain environment variables, so expand them. */
                            if (reader->debugIncludes) {
                                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS,
                                    TEXT("Found include file in %s: %s"), filename, c);
                            }

                            if (expandVars) {
                                evaluateEnvironmentVariables(c, expBuffer, MAX_PROPERTY_NAME_VALUE_LENGTH, logWarnings, warnedVarMap, logWarningLogLevel, ignoreVarMap, FALSE, NULL, NULL, NULL, CIPHER_FORBID | VAR_EXPAND);

                                if (reader->debugIncludes && (_tcscmp(c, expBuffer) != 0)) {
                                    /* Only show this log if there were any environment variables. */
                                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS,
                                        TEXT("  After environment variable replacements: %s"), expBuffer);
                                }
                            } else {
                                _tcsncpy(expBuffer, c, MAX_PROPERTY_NAME_VALUE_LENGTH);
                            }
                            
                            /* Now obtain the real absolute path to the include file. */
#ifdef WIN32
                            /* Find out how big the absolute path will be */
                            /*  Note: Unlike _trealpathN() on Unix, GetFullPathName() does not check if the path is valid or exists.
                             *        So the function will pretty much only fail in case of a memory error or if the path is too long, etc.
                             *        However, the actual reading of the file will fail when we call configFileReader_Read() later below. */
                            size = GetFullPathName(expBuffer, 0, NULL, NULL); /* Size includes '\0' */
                            if (!size) {
                                absoluteBuffer = NULL;
                            } else {
                                absoluteBuffer = malloc(sizeof(TCHAR) * size);
                                if (!absoluteBuffer) {
                                    outOfMemory(TEXT("RCF"), 2);
                                } else {
                                    if (!GetFullPathName(expBuffer, size, absoluteBuffer, NULL)) {
                                        free(absoluteBuffer);
                                        absoluteBuffer = NULL;
                                    }
                                }
                            }
#else
                            absoluteBuffer = malloc(sizeof(TCHAR) * (PATH_MAX + 1));
                            if (!absoluteBuffer) {
                                outOfMemory(TEXT("RCF"), 3);
                            } else {
                                if (_trealpathN(expBuffer, absoluteBuffer, PATH_MAX + 1) == NULL) {
                                    free(absoluteBuffer);
                                    absoluteBuffer = NULL;
                                }
                            }
#endif
                            if (!absoluteBuffer) {
                                if (includeRequired) {
                                    if (reader->minLogLevel <= LEVEL_ERROR) {
                                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR,
                                            TEXT("Unable to resolve the full path of included configuration file: %s (%s)\n  Referenced from: %s (line %d)\n  Current working directory: %s"),
                                            expBuffer, getLastErrorText(), filename, lineNumber, originalWorkingDir);
                                    }
                                    readResult = CONFIG_FILE_READER_FAIL;
                                    free(bufferW);  /* Include directives shouldn't contain sensitive data, no need to fill the buffer with zeros. */
                                    break;
                                } else if (reader->debugIncludes && (reader->minLogLevel <= LEVEL_STATUS)) {
                                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS,
                                        TEXT("Unable to resolve the full path of included configuration file: %s (%s)\n  Referenced from: %s (line %d)\n  Current working directory: %s"),
                                        expBuffer, getLastErrorText(), filename, lineNumber, originalWorkingDir);
                                }
                            } else {
                                if (depth < MAX_INCLUDE_DEPTH) {
                                    readResult = configFileReader_Read(reader, absoluteBuffer, includeRequired, depth + 1, filename, lineNumber, originalWorkingDir, warnedVarMap, ignoreVarMap, logWarnings, logWarningLogLevel);
                                    if (readResult == CONFIG_FILE_READER_SUCCESS) {
                                        /* Ok continue. */
                                    } else if ((readResult == CONFIG_FILE_READER_FAIL) || (readResult == CONFIG_FILE_READER_OPEN_FAIL)) {
                                        /* Failed. */
                                        if (includeRequired) {
                                            /* Include file was required, but we failed to read it. */
                                            if (readResult == CONFIG_FILE_READER_FAIL) {
                                                /* An error message was logged, but it did not specify that it was an include file. */
                                                if (reader->minLogLevel <= LEVEL_ERROR) {
                                                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR,
                                                        TEXT("%sThe required configuration file, %s, was not read.\n%s  Referenced from: %s (line %d)"),
                                                        (reader->debugIncludes ? TEXT("  ") : TEXT("")), expBuffer, (reader->debugIncludes ? TEXT("  ") : TEXT("")), filename, lineNumber);
                                                }
                                            } else {
                                                /* An error message was logged and it already specified that it was an included file. */
                                                readResult = CONFIG_FILE_READER_FAIL;
                                            }
                                        }
                                        if (readResult == CONFIG_FILE_READER_FAIL) {
                                            /* Can't continue. */
                                            free(absoluteBuffer);
                                            free(bufferW);  /* Include directives shouldn't contain sensitive data, no need to fill the buffer with zeros. */
                                            break;
                                        } else {
                                            /* Failed but continue. */
                                            readResult = CONFIG_FILE_READER_SUCCESS;
                                        }
                                    } else {
                                        _tprintf(TEXT("Unexpected load error %d\n"), readResult);
                                        /* continue. */
                                        readResult = CONFIG_FILE_READER_SUCCESS;
                                    }
                                } else {
                                    if (reader->debugIncludes) {
                                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_STATUS,
                                            TEXT("  Unable to include configuration file, %s, because the max include depth was reached."), absoluteBuffer);
                                    }
                                }
                                free(absoluteBuffer);
                            }
                        }
                    }
                } else if (_tcsstr(trimmedBuffer, TEXT("properties.")) == trimmedBuffer) {
                    onOverwriteChanged = FALSE;

                    if(_tcsstr(trimmedBuffer, TEXT("properties.on_overwrite.exit=")) == trimmedBuffer) {
                        trimmedBuffer += 29;
                        if (_tcsicmp(trimmedBuffer, TEXT("TRUE")) == 0) {
                            reader->exitOnOverwrite = TRUE;
                            onOverwriteChanged = TRUE;
                        } else if (_tcsicmp(trimmedBuffer, TEXT("FALSE")) == 0) {
                            reader->exitOnOverwrite = FALSE;
                            onOverwriteChanged = TRUE;
                        } else if (reader->minLogLevel <= LEVEL_WARN) {
                            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN,
                                TEXT("Encountered an invalid boolean value for directive %c%s=%s (line %d).  Ignoring this directive."),
                                directiveChar, TEXT("properties.on_overwrite.exit"), trimmedBuffer, lineNumber);
                        }
                    } else if (_tcsstr(trimmedBuffer, TEXT("properties.on_overwrite.loglevel=")) == trimmedBuffer) {
                        trimmedBuffer += 33;
                        if (_tcsicmp(trimmedBuffer, TEXT("AUTO")) == 0) {
                            reader->logLevelOnOverwrite = -1;
                            onOverwriteChanged = TRUE;
                        } else {
                            reader->logLevelOnOverwrite = getLogLevelForName(trimmedBuffer);
                            if (reader->logLevelOnOverwrite != LEVEL_UNKNOWN) {
                                onOverwriteChanged = TRUE;
                                if (reader->logLevelOnOverwrite >= LEVEL_NONE) {
                                    /* At least log with LEVEL_DEBUG to help support. */
                                    reader->logLevelOnOverwrite = LEVEL_DEBUG;
                                }
                            } else {
                                reader->logLevelOnOverwrite = logLevelOnOverwrite;
                                if (reader->minLogLevel <= LEVEL_WARN) {
                                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN,
                                        TEXT("Encountered an invalid value for directive %c%s=%s (line %d).  Ignoring this directive."),
                                        directiveChar, TEXT("properties.on_overwrite.loglevel=%s"), trimmedBuffer, lineNumber);
                                }
                            }
                        }
                    } else if (strcmpIgnoreCase(trimmedBuffer, TEXT("properties.debug")) == 0) {
                        reader->logLevelOnOverwrite = LEVEL_STATUS;
                        onOverwriteChanged = TRUE;
                    }
                    if (onOverwriteChanged && reader->debugCallback) {
                        (*reader->debugCallback)(reader->callbackParam, reader->exitOnOverwrite, reader->logLevelOnOverwrite);
#ifdef _DEBUG
                        if (reader->exitOnOverwrite != exitOnOverwrite) {
                            _tprintf(TEXT("on_overwrite.exit: %s\n  file: %s (%d)\n"),
                                reader->exitOnOverwrite ? TEXT("TRUE") : TEXT("FALSE"),
                                filename, lineNumber);
                        }
                        if (reader->logLevelOnOverwrite != logLevelOnOverwrite) {
                            _tprintf(TEXT("on_overwrite.loglevel: %s\n  file: %s (%d)\n"),
                                reader->logLevelOnOverwrite == -1 ? TEXT("AUTO") : logLevelNames[reader->logLevelOnOverwrite],
                                filename, lineNumber);
                        }
#endif
                    }
                } else if (_tcsstr(trimmedBuffer, TEXT("log_messages.buffer_size=")) == trimmedBuffer) {
                    trimmedBuffer += 25;
                    setThreadMessageBufferInitialSize(_ttoi(trimmedBuffer));
                } else if (_tcsstr(trimmedBuffer, TEXT("variables.expand=")) == trimmedBuffer) {
                    trimmedBuffer += 17;
                    if (_tcsicmp(trimmedBuffer, TEXT("TRUE")) == 0) {
                        expandVars = TRUE;
                    } else if (_tcsicmp(trimmedBuffer, TEXT("FALSE")) == 0) {
                        expandVars = FALSE;
                    } else if (reader->minLogLevel <= LEVEL_WARN) {
                        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_WARN,
                            TEXT("Encountered an invalid boolean value for directive %c%s=%s (line %d).  Ignoring this directive."),
                            directiveChar, TEXT("variables.expand"), trimmedBuffer, lineNumber);
                    }
                } else if (directiveChar == TEXT('@')) {
                    /* Unknown directive, or first character of a parameter (for a parameter file) */
                    trimmedBuffer--;
                    delegateParsing = TRUE;
                }
            } else {
                delegateParsing = TRUE;
            }

            if (delegateParsing) {
                if (!(*reader->callback)(reader->callbackParam, filename, lineNumber, depth, trimmedBuffer, expandVars, reader->minLogLevel)) {
                    readResult = CONFIG_FILE_READER_FAIL;
                    wrapperSecureFree(bufferW, bufferWSize);
                    break;
                }
            }
        }
            
        /* Always free each line read. */
        wrapperSecureFree(bufferW, bufferWSize);

        readLineRet = readConfigLine(bufferMB, stream, reader, &lineNumber);
    }
    
    /* Always fill the buffer with 0 when finished. */
    wrapperSecureZero(bufferMB, sizeof(bufferMB));
    
    /* Close the file */
    fclose(stream);

    if ((depth > 0) && ((reader->exitOnOverwrite != exitOnOverwrite) || (reader->logLevelOnOverwrite != logLevelOnOverwrite))) {
        (*reader->debugCallback)(reader->callbackParam, exitOnOverwrite, logLevelOnOverwrite);
    }
#ifdef _DEBUG
    if (!reader->preload) {
        if (reader->includeRequiredByDefault != includeRequiredByDefault) {
            _tprintf(TEXT("include mode restored : %s -> %s\n  file: %s (%d)\n"),
                reader->includeRequiredByDefault ? TEXT("required") : TEXT("optional"),
                includeRequiredByDefault         ? TEXT("required") : TEXT("optional"),
                filename, lineNumber);
        }
        if (reader->debugIncludes != debugIncludes) {
            _tprintf(TEXT("debug include restored: %s -> %s\n  file: %s (%d)\n"),
                reader->debugIncludes ? TEXT("TRUE") : TEXT("FALSE"),
                debugIncludes         ? TEXT("TRUE") : TEXT("FALSE"),
                filename, lineNumber);
        }
        if (reader->exitOnOverwrite != exitOnOverwrite) {
            _tprintf(TEXT("on_overwrite.exit restored: %s -> %s\n  file: %s (%d)\n"),
                reader->exitOnOverwrite ? TEXT("TRUE") : TEXT("FALSE"),
                exitOnOverwrite         ? TEXT("TRUE") : TEXT("FALSE"),
                filename, lineNumber);
        }
        if (reader->logLevelOnOverwrite != logLevelOnOverwrite) {
            _tprintf(TEXT("on_overwrite.loglevel restored: %s -> %s\n  file: %s (%d)\n"),
                reader->logLevelOnOverwrite == -1 ? TEXT("AUTO") : logLevelNames[reader->logLevelOnOverwrite],
                logLevelOnOverwrite == -1 ? TEXT("AUTO") : logLevelNames[logLevelOnOverwrite],
                filename, lineNumber);
        }
    }
#endif

    reader->includeRequiredByDefault = includeRequiredByDefault;
    reader->debugIncludes = debugIncludes;
    reader->exitOnOverwrite = exitOnOverwrite;
    reader->logLevelOnOverwrite = logLevelOnOverwrite;

    return readResult;
}

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
int configFileReader(const TCHAR *filename,
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
                     int logWarningLogLevel) {
    ConfigFileReader reader;
    
    /* Initialize the reader. */
    reader.callback = callback;
    reader.callbackParam = callbackParam;
    reader.readFilterCallback = readFilterCallback;
    reader.debugCallback = debugCallback;
    reader.paramFilePropName = paramFilePropName;
    reader.enableIncludes = enableIncludes;
    reader.preload = preload;
    reader.minLogLevel = minLogLevel;
    reader.debugIncludes = FALSE;
    reader.includeRequiredByDefault = FALSE;
    reader.exitOnOverwrite = FALSE;
    reader.logLevelOnOverwrite = -1;    /* AUTO */
    
    return configFileReader_Read(&reader, filename, fileRequired, 0, NULL, 0, originalWorkingDir, warnedVarMap, ignoreVarMap, logWarnings, logWarningLogLevel);
}
