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

#ifdef WIN32
 #ifndef _WRAPPER_WINEVT_H
 #define _WRAPPER_WINEVT_H

 #include <windows.h>
 #include <sddl.h>
 #include <stdio.h>

 #define EvtQueryChannelPath         0x1
 #define EvtQueryReverseDirection    0x200

 #define EvtRenderContextValues      0

 #define EvtRenderEventValues        0

typedef HANDLE EVT_HANDLE;

typedef struct _EVT_VARIANT {
  union {
    BOOL       BooleanVal;
    INT8       SByteVal;
    INT16      Int16Val;
    INT32      Int32Val;
    INT64      Int64Val;
    UINT8      ByteVal;
    UINT16     UInt16Val;
    UINT32     UInt32Val;
    UINT64     UInt64Val;
    float      SingleVal;
    double     DoubleVal;
    ULONGLONG  FileTimeVal;
    SYSTEMTIME *SysTimeVal;
    GUID       *GuidVal;
    LPCWSTR    StringVal;
    LPCSTR     AnsiStringVal;
    PBYTE      BinaryVal;
    PSID       SidVal;
    size_t     SizeTVal;
    BOOL       *BooleanArr;
    INT8       *SByteArr;
    INT16      *Int16Arr;
    INT32      *Int32Arr;
    INT64      *Int64Arr;
    UINT8      *ByteArr;
    UINT16     *UInt16Arr;
    UINT32     *UInt32Arr;
    UINT64     *UInt64Arr;
    float      *SingleArr;
    double     *DoubleArr;
    FILETIME   *FileTimeArr;
    SYSTEMTIME *SysTimeArr;
    GUID       *GuidArr;
    LPWSTR     *StringArr;
    LPSTR      *AnsiStringArr;
    PSID       *SidArr;
    size_t     *SizeTArr;
    EVT_HANDLE EvtHandleVal;
    LPCWSTR    XmlVal;
    LPCWSTR    *XmlValArr;
  };
  DWORD Count;
  DWORD Type;
} EVT_VARIANT, *PEVT_VARIANT;

static HMODULE wevtapiMod;
static FARPROC EvtQuery = NULL;
static FARPROC EvtNext = NULL;
static FARPROC EvtClose = NULL;
static FARPROC EvtCreateRenderContext = NULL;
static FARPROC EvtRender = NULL;

 #endif
#endif
