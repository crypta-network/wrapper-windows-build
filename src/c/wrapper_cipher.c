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
#ifndef WIN32
 #include <string.h>
#endif

#include "logger.h"
#include "wrapper_i18n.h"
#include "wrapper_cipher.h"

static void reportInvalidCipherToken(const TCHAR* src, TCHAR* nameStart, size_t nameLen) {
    TCHAR* cipherName = NULL;

    if (nameLen <= 0) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Invalid cipher found in %s (name was empty)."), src);
    } else {
        cipherName = malloc(sizeof(TCHAR) * (nameLen + 1));
        if (!cipherName) {
            outOfMemory(TEXT("RIC"), 1);
        } else {
            _tcsncpy(cipherName, nameStart, nameLen);
            cipherName[nameLen] = 0;
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Invalid cipher name '%s' found in %s."), cipherName, src);
            free(cipherName);
        }
    }
}

void maskBuffer(TCHAR* buffer, size_t len) {
    size_t i;

    for (i = 0; i < len; i++) {
        buffer[i] = TEXT('*');
    }
    buffer[len] = TEXT('\0');
}

static void maskCipher(const TCHAR* src, TCHAR* codeStart, size_t codeLen, TCHAR* nameStart, size_t nameLen, int isValid, TCHAR* out) {
    maskBuffer(out, WRAPPER_CIPHER_MASK_LEN);
}

void maskSensitiveData(const TCHAR* value, TCHAR** pBuffer) {
    readCiphers(NULL, value, maskCipher, pBuffer, FALSE);
}

static void unmaskCipher(const TCHAR* src, TCHAR* codeStart, size_t codeLen, TCHAR* nameStart, size_t nameLen, int isValid, TCHAR* out) {
    if (!isValid) {
        reportInvalidCipherToken(src, nameStart, nameLen);
    } else {
        _tcsncpy(out, codeStart, codeLen);
        out[codeLen] = TEXT('\0');
    }
}

void decipherSensitiveData(const TCHAR* src, const TCHAR* value, TCHAR** pBuffer) {
    readCiphers(src, value, unmaskCipher, pBuffer, FALSE);
}
