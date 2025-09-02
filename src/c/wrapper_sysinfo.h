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
 *   Leif Mortenson <leif@tanukisoftware.com>
 */

#ifndef _WRAPPER_SYSINFO_H
#define _WRAPPER_SYSINFO_H

#include "wrapper_i18n.h"

 #if defined(LINUX) || defined(MACOSX) || defined(FREEBSD)
  #define CHECK_FILE_SYSTEM_SUPPORTED
 #endif

 #if !defined(WIN32) && defined(CHECK_FILE_SYSTEM_SUPPORTED)
int isSafeFileSystem(const TCHAR* path);
 #endif
#endif
