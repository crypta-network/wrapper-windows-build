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

#include <stdio.h>
#include <stdlib.h>

#include "wrapper_sysinfo.h"

#ifdef CHECK_FILE_SYSTEM_SUPPORTED
 /* header for statfs */
 #if defined(MACOSX) || defined(FREEBSD)
  #include <strings.h>
  #include <sys/mount.h>
 #elif defined(LINUX)
  #include <sys/vfs.h>
 #else
  /* Actually not used, but would be the header to use on systems like Solaris, AIX, etc. */
  #include <sys/statfs.h>
 #endif
#endif

#include "logger.h"
#include "wrapper_i18n.h"

#ifdef CHECK_FILE_SYSTEM_SUPPORTED
int isSafeFileSystem(const TCHAR* path) {
    char *pathMb = NULL;
    struct statfs buffer;

    if (converterWideToMB(path, &pathMb, __UTF8) < 0) {
        if (pathMb) {
            free(pathMb);
        } else {
            outOfMemory(TEXT("ISFS"), 1);
        }
        return FALSE;
    }

    if (statfs(pathMb, &buffer) != 0) {
        free(pathMb);
        return FALSE;
    }
    free(pathMb);

    /*    Note: The lists below are not exhaustive, so improve as needed. If a file system is not present in the list, it will be considered as "unsecured".
     *          The user can then either adjust the permissions or set wrapper.secure_file.check.*.allow_unsecured_volumes to TRUE. */
 #if defined(MACOSX) || defined(FREEBSD)
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Checking file system type for %s: % s"), path, buffer.f_fstypename);
    if ((strIgnoreCaseCmp(buffer.f_fstypename, "apfs") == 0) ||
        (strIgnoreCaseCmp(buffer.f_fstypename, "hfs") == 0) ||
        (strIgnoreCaseCmp(buffer.f_fstypename, "ufs") == 0) ||
        (strIgnoreCaseCmp(buffer.f_fstypename, "nfs") == 0) ||
        (strIgnoreCaseCmp(buffer.f_fstypename, "smbfs") == 0) ||
        (strIgnoreCaseCmp(buffer.f_fstypename, "tmpfs") == 0) ||
        (strIgnoreCaseCmp(buffer.f_fstypename, "devfs") == 0) ||
        (strncasecmp(buffer.f_fstypename, "ext", 3) == 0)) {
        return TRUE;
    } else {
        return FALSE;
    }
 #else
    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Checking file system type for %s: %d"), path, buffer.f_type);
    switch (buffer.f_type) {
        case 0x9123683E: /* btrfs */
        case 0xEF53:     /* ext2, ext3, ext4 */
        case 0x65735546: /* FUSE */
        case 0x3153464A: /* JFS */
        case 0x6969:     /* NFS */
        case 0x00011954: /* UFS */
        case 0x58465342: /* XFS */
        case 0x2FC12FC1: /* ZFS */
            return TRUE;
        default:
            return FALSE;
    }
 #endif
}
#endif
