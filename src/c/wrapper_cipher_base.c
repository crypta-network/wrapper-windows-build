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

#include <stdio.h>
#include <stdlib.h>
#ifdef WIN32
 #include <windows.h>
#else
 #include <string.h>
#endif

#include "logger_base.h"
#include "wrapper_i18n.h"
#include "wrapper_cipher_base.h"

/**
 * Base function to read a string and execute a callback on each cipher found.
 *  If pOut is specified and any ciphers are found, the function will set it by
 *  decoding all ciphers in the input string.
 *
 * @param src      Optional name of the property or filename where the value is read.
 *                 This parameter is only intended for use in the callback.
 * @param in       Input string to read.
 * @param callback Callback to be called on each cipher found.
 * @param pOut     Optional pointer to a retrieve the decoded output. pOut is
 *                 only set if cipher are found. This allows the caller to
 *                 choose whether to realloc the value or ignore this call.
 * @param expandPercentage TRUE to expand %WRAPPER_PERCENTAGE% (only needed
 *                         when calling from the native library).
 */
void readCiphers(const TCHAR* src, const TCHAR* in, ProcessCipherCallback callback, TCHAR** pOut, int expandPercentage) {
    TCHAR *start;
    TCHAR *end;
    TCHAR *pipe;
    TCHAR *out = NULL;
    TCHAR *outPtr = NULL;
    size_t outLen;
    size_t cipherNameLen;
    size_t cipherCodeLen;
    int isValid;

    /* Loop until we hit the end of string. */
    while (in[0] != TEXT('\0')) {
        start = _tcschr(in, TEXT('%'));
        if (start) {
            end = _tcschr(start + 1, TEXT('%'));
            if (end) {
                /* A pair of '%' characters was found. */
                pipe = _tcschr(start + 1, TEXT('|'));
                if (pipe && (pipe < end)) {
                    isValid = TRUE;
                    cipherCodeLen = pipe - start - 1;
                    cipherNameLen = end - pipe - 1;
                    if (cipherNameLen <= 0) {
                        isValid = FALSE;
                    } else {
                        if ((_tcsnicmp(pipe + 1, TEXT("mask"), cipherNameLen) != 0)) {
                            isValid = FALSE;
                        }
                    }

                    if (pOut) {
                        if (!out) {
                            /* A decoded cipher is always shorter than its encoded form. */
                            out = malloc(sizeof(TCHAR) * (_tcslen(in) + 1));
                            if (!out) {
                                outOfMemory(TEXT("RC"), 1);
                                return;
                            }
                            outPtr = out;
                        }
                        /* Copy over any text before the cipher token */
                        outLen = (start - in);
                        if (outLen > 0) {
                            _tcsncpy(outPtr, in, outLen);
                            outPtr += outLen;
                        }

                        /* Terminate the string */
                        outPtr[0] = TEXT('\0');
                    }

                    if (callback) {
                        /* Process the cipher */
                        callback(src, start + 1, cipherCodeLen, pipe + 1, cipherNameLen, isValid, pOut ? outPtr : NULL);
                        if (outPtr) {
                            outPtr += _tcslen(outPtr);
                        }
                    }

                    /* Set the new in pointer */
                    in = end + 1;
                } else {
                    if (pOut) {
                        /* This is an unexpanded variable */
                        if (expandPercentage && (_tcsncmp(start + 1, TEXT("WRAPPER_PERCENTAGE"), end - start - 1) == 0)) {
                            /* This percentage was escaped and needs to be expanded.
                             *  Note: This is only needed when reading ciphers from the native library.  When loading
                             *        the configuration, the percentages are always expanded after reading the ciphers). */
                            if (!out) {
                                out = malloc(sizeof(TCHAR) * (_tcslen(in) - 19 + 1));
                                if (!out) {
                                    outOfMemory(TEXT("RC"), 2);
                                    return;
                                }
                                outPtr = out;
                            }

                            /* Copy over any text before the variable */
                            outLen = (start - in);
                            if (outLen > 0) {
                                _tcsncpy(outPtr, in, outLen);
                                outPtr += outLen;
                            }

                            /* Copy the percentage */
                            outPtr[0] = TEXT('%');
                            outPtr++;

                            /* Terminate the string */
                            outPtr[0] = TEXT('\0');
                        } else if (outPtr) {
                            /* Leave the variable as is. Copy over any text until the end of the variable */
                            outLen = (end + 1 - in);
                            if (outLen > 0) {
                                _tcsncpy(outPtr, in, outLen);
                                outPtr += outLen;
                            }

                            /* Terminate the string */
                            outPtr[0] = TEXT('\0');
                        }
                    }

                    /* Set the new in pointer */
                    in = end + 1;
                }
            } else {
                /* Single '%' found */
                break;
            }
        } else {
            /* No more '%' */
            break;
        }
    }

    if (outPtr) {
        /* Copy the rest */
        outLen = _tcslen(in);
        if (outLen > 0) {
            _tcsncpy(outPtr, in, outLen);
            outPtr += outLen;
        }

        /* Terminate the string */
        outPtr[0] = TEXT('\0');

        /* Set output parameter */
        *pOut = out;
    }
}
