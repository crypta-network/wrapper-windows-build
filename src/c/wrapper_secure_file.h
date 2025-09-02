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
#ifndef _WRAPPER_SECURE_FILE_H
#define _WRAPPER_SECURE_FILE_H

#include "wrapper_i18n.h"

 #ifndef WIN32
  #define SHIFT_OWNER           6
  #define SHIFT_GROUP           3
  #define SHIFT_OTHERS          0
 #endif

typedef struct SecureFilePermission SecureFilePermission;
struct SecureFilePermission {
    TCHAR* accountName;
    int isAllowedAccount;
    int isForbiddenAccount;
 #ifdef WIN32
    PSID pSid;
 #else
    id_t id;
    int classShift;
 #endif
    int mask;
    SecureFilePermission *next;
};

typedef struct SecureFilePermissionList SecureFilePermissionList;
struct SecureFilePermissionList {
    SecureFilePermission *first;
    SecureFilePermission *last;
};

typedef struct InsecureFile InsecureFile;
struct InsecureFile {
    TCHAR* path;
    SecureFilePermissionList* permissions;
    int isOnUnsecuredVolume;
    int critical;
    InsecureFile *next;
};

typedef struct InsecureFileList InsecureFileList;
struct InsecureFileList {
    InsecureFile *first;
    InsecureFile *last;
};

int checkFilePermissions(const TCHAR* path);

int checkAndReportInsecureFiles();

void disposeSecureFiles();

int resetSecureFileChecks();

int loadSecureFileConfiguration();

#endif
