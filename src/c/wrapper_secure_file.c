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
 #include <lmcons.h>
#else
 #include <string.h>
 #include <errno.h>
 #include <grp.h>
#endif

#include "logger.h"
#include "wrapper.h"
#include "wrapper_i18n.h"
#ifndef WIN32
 #include "wrapper_sysinfo.h"
#endif
#include "wrapper_secure_file.h"

/* Important: These constants define values for checkedFilesVarMap. FILE_NOT_CHECKED must be equal to 0 which is the value returned when a key doesn't exist in the hashmap. */
#define FILE_NOT_CHECKED    0
#define FILE_INSECURE       1
#define FILE_SECURE         2

#ifdef WIN32
 #define ACCESS_READ     1179785
 #define ACCESS_WRITE    1048854
#else

static char** allowedAccountOrGroupNamesMb = NULL;
#endif

static TCHAR** allowedAccountOrGroupNames = NULL;

static int allowUnsecuredVolumes = FALSE;

static PHashMap checkedFilesVarMap = NULL;

static InsecureFileList* insecureFiles = NULL;

void disposeSecureFilePermission(SecureFilePermission* permission) {
    if (permission) {
        free(permission->accountName);
#ifdef WIN32
        free(permission->pSid);
#endif
        free(permission);
    }
}

void disposeSecureFilePermissionList(SecureFilePermissionList* list) {
    SecureFilePermission* permission;
    SecureFilePermission* tempPermission;

    if (list) {
        permission = list->first;
        while (permission) {
            tempPermission = permission->next;
            disposeSecureFilePermission(permission);
            permission = tempPermission;
        }
        free(list);
    }
}

void disposeInsecureFile(InsecureFile* insecureFile) {
    if (insecureFile) {
        free(insecureFile->path);
        disposeSecureFilePermissionList(insecureFile->permissions);
        free(insecureFile);
    }
}

void disposeInsecureFileList(InsecureFileList* list) {
    InsecureFile* insecureFile;
    InsecureFile* tempInsecureFile;

    if (list) {
        insecureFile = list->first;
        while (insecureFile) {
            tempInsecureFile = insecureFile->next;
            disposeInsecureFile(insecureFile);
            insecureFile = tempInsecureFile;
        }
        free(list);
    }
}

int addToInsecureFileList(const TCHAR* path, int critical, SecureFilePermissionList* permissions, int unsecuredVolume) {
    InsecureFile* insecureFile;

    if (!insecureFiles) {
        insecureFiles = malloc(sizeof(InsecureFileList));
        if (!insecureFiles) {
            outOfMemory(TEXT("ATIFL"), 1);
            return TRUE;
        } else {
            insecureFiles->first = NULL;
            insecureFiles->last = NULL;
        }
    }
    insecureFile = malloc(sizeof(InsecureFile));
    if (!insecureFile) {
        outOfMemory(TEXT("ATIFL"), 2);
        return TRUE;
    } else {
        insecureFile->path = NULL;
        updateStringValue(&insecureFile->path, path);
        insecureFile->critical = critical;
        insecureFile->permissions = permissions;
        insecureFile->isOnUnsecuredVolume = unsecuredVolume;
        insecureFile->next = NULL;

        if (insecureFiles->last) {
            insecureFiles->last->next = insecureFile;
        }
        insecureFiles->last = insecureFile;
        if (!insecureFiles->first) {
            insecureFiles->first = insecureFile;
        }
    }
    return FALSE;
}

#ifdef WIN32
static PSECURITY_DESCRIPTOR getSecurityDescriptor(const TCHAR* path, SECURITY_INFORMATION si) {
    PSECURITY_DESCRIPTOR sd = NULL;
    DWORD len = 0;
    DWORD lenNeeded = 0;

    if (!GetFileSecurity(path, si, sd, len, &lenNeeded)) {
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Failed to retrieve security information for %s. %s"), path, getLastErrorText());
            return NULL;
        }

        sd = (PSECURITY_DESCRIPTOR)LocalAlloc(LMEM_FIXED, lenNeeded);
        if (sd) {
            len = lenNeeded;
            if (!GetFileSecurity(path, si, sd, len, &lenNeeded)) {
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Failed to retrieve security information for %s. %s"), path, getLastErrorText());
                LocalFree((HLOCAL)sd);
                return NULL;
            }
        }
    }
    return sd;
}

/**
 */
static int isForbiddenAccount(PSID pSID) {
    PSID_IDENTIFIER_AUTHORITY pSia;
    DWORD x;

    pSia = GetSidIdentifierAuthority(pSID);

    x = (DWORD)((pSia->Value[2] << 24) +
                (pSia->Value[3] << 16) +
                (pSia->Value[4] << 8) +
                (pSia->Value[5]));

    /* Null SID */
    if (x == 0) {
        return TRUE;
    }

    /* World or Everyone */
    if (x == 1) {
        return TRUE;
    }

    /* ANONYMOUS LOGON */
    if (*GetSidSubAuthority(pSID, 0) == SECURITY_ANONYMOUS_LOGON_RID) {
        return TRUE;
    }

    /* Authenticated users */
    if (*GetSidSubAuthority(pSID, 0) == SECURITY_AUTHENTICATED_USER_RID) {
        return TRUE;
    }

    if (*GetSidSubAuthorityCount(pSID) >= 2) {
        /* Guests */
        if ((*GetSidSubAuthority(pSID, 1) == DOMAIN_ALIAS_RID_GUESTS)) {
            return TRUE;
        }

        /* Users */
        if ((*GetSidSubAuthority(pSID, 1) == DOMAIN_ALIAS_RID_USERS)) {
            return TRUE;
        }
    }

    return FALSE;
}

static SecureFilePermission* addSecureFilePermission(SecureFilePermissionList* list, ACCESS_ALLOWED_ACE* pAce) {
    SecureFilePermission* p;
    TCHAR accName[UNLEN + 1];
    TCHAR domainName[DNLEN + 1];
    DWORD cchAccName;
    DWORD cchDomainName;
    SID_NAME_USE eSidType;
    size_t len;
    DWORD dwSidSize;

    p = malloc(sizeof(SecureFilePermission));
    if (!p) {
        outOfMemory(TEXT("ASFP"), 1);
    } else {
        cchAccName = UNLEN + 1;
        cchDomainName = DNLEN + 1;

        p->accountName = NULL;
        if (LookupAccountSid(NULL,
                              &pAce->SidStart,
                              accName,
                              &cchAccName,
                              domainName,
                              &cchDomainName,
                              &eSidType)) {
            len = _tcslen(accName);
            if (len > 0) {
                if (domainName && (domainName[0] != 0)) {
                    len += 1 + _tcslen(domainName);
                    p->accountName = malloc(sizeof(TCHAR) * (len + 1));
                    if (!p->accountName) {
                        outOfMemory(TEXT("ASFP"), 2);
                        disposeSecureFilePermission(p);
                        return NULL;
                    } else {
                        _sntprintf(p->accountName, len + 1, TEXT("%s\\%s"), domainName, accName);
                    }
                } else {
                    updateStringValue(&p->accountName, accName);
                }
            }
        }
        if (p->accountName) {
            /* We need to work on the copy of the SID because pAce can be deallocated. */
            dwSidSize = GetLengthSid(&pAce->SidStart);
            p->pSid = malloc(dwSidSize);
            if (!p->pSid) {
                outOfMemory(TEXT("ASFP"), 3);
                disposeSecureFilePermission(p);
                return NULL;
            } else {
                CopySid(dwSidSize, p->pSid, &pAce->SidStart);
                p->isForbiddenAccount = isForbiddenAccount(p->pSid);
            }
        } else {
            /* Account name resolution failed. */
            updateStringValue(&p->accountName, TEXT("<unknown>"));
            p->pSid = NULL;
            p->isForbiddenAccount = FALSE;  /* Arguably could be forbidden, but there would be no way to continue. If we decide to be more strict, keep consistent with Unix. */
        }

        p->isAllowedAccount = FALSE; /* will be set to TRUE if present in the conf list */
        p->mask = pAce->Mask;
        p->next = NULL;

        if (list->last) {
            list->last->next = p;
        }
        list->last = p;
        if (!list->first) {
            list->first = p;
        }
    }

    return p;
}

int checkFilePermissions(const TCHAR* path) {
    FILE* file;
    PSECURITY_DESCRIPTOR pSD = NULL;
    BOOL bDaclPresent = FALSE;
    BOOL bDaclDefaulted = FALSE;
    PACL pAcl = NULL;
    ACL_SIZE_INFORMATION aclsizeinfo;
    SecureFilePermissionList* permissions = NULL;
    SecureFilePermission* p;
    DWORD cAce;
    ACCESS_ALLOWED_ACE *pAce = NULL;
    PSID pSidAllowed = NULL;
    int i;
    int found;
    int reportPermissions = FALSE;
    int failed = FALSE;

    switch (hashMapGetKWVI(checkedFilesVarMap, path)) {
    case FILE_INSECURE:
        return TRUE;

    case FILE_SECURE:
        return FALSE;

    case FILE_NOT_CHECKED:
    default:
        break;
    }

    /* First collect the permissions. If any error occurs, it will be reported below. */
    pSD = getSecurityDescriptor(path, GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION);
    if (!pSD) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Failed to retrieve the security descriptor of %s. %s"), path, getLastErrorText());
        failed = TRUE;
    } else {
        if (!GetSecurityDescriptorDacl(pSD, &bDaclPresent, &pAcl, &bDaclDefaulted)) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Failed to retrieve DACL of %s. %s"), path, getLastErrorText());
            failed = TRUE;
        } else if (!bDaclPresent || !pAcl) {
            /* This can happen on a non-NTFS partition (unsecured volume). */
        } else {
            ZeroMemory(&aclsizeinfo, sizeof(aclsizeinfo));
            if (!GetAclInformation(pAcl, &aclsizeinfo, sizeof(aclsizeinfo), AclSizeInformation)) {
                log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Failed to retrieve information about the DACL of %s. %s"), path, getLastErrorText());
                failed = TRUE;
            } else {
                permissions = malloc(sizeof(SecureFilePermissionList));
                if (!permissions) {
                    outOfMemory(TEXT("CFP"), 1);
                    failed = TRUE;
                } else {
                    memset(permissions, 0, sizeof(SecureFilePermissionList));
                    for (cAce = 0; cAce < aclsizeinfo.AceCount; cAce++) {
                        if (!GetAce(pAcl, cAce, (LPVOID*)&pAce)) {
                            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_DEBUG, TEXT("Failed to retrieve an ACE for %s. %s"), path, getLastErrorText());
                            failed = TRUE;
                            break;
                        }
                        if (!(pAce->Header.AceType & ACCESS_DENIED_ACE_TYPE)) {
                            if (((pAce->Mask & ACCESS_READ) == ACCESS_READ) || ((pAce->Mask & ACCESS_WRITE) == ACCESS_WRITE)) {
                                if (!addSecureFilePermission(permissions, pAce)) {
                                    failed = TRUE;
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        }
        LocalFree((HLOCAL)pSD);
    }

    if (failed) {
        file = _tfopen(path, TEXT("r"));
        if (!file) {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Failed to check permissions of %s. %s"), path, TEXT("The file does not exist."));
        } else {
            log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Failed to check permissions of %s. %s"), path, TEXT("Failed to retrieve existing permissions."));
            fclose(file);
        }
    } else {
        /* Now, check if the permissions are allowed. */
        if (!permissions) {
            /* Unsecured volume */
            if (!allowUnsecuredVolumes) {
                failed |= wrapperData->secureFileCheckStrict;
                addToInsecureFileList(path, failed, NULL, TRUE);
            }
        } else {
            for (p = permissions->first; p; p = p->next) {
                if (wrapperData->secureFileCheckDisabled) {
                    p->isAllowedAccount = TRUE;
                } else {
                    if (p->pSid) {
                        for (i = 0; allowedAccountOrGroupNames[i]; i++) {
                            getTrusteeSidFromName(allowedAccountOrGroupNames[i], &pSidAllowed);
                            if (pSidAllowed) {
                                found = EqualSid(p->pSid, pSidAllowed);
                                free(pSidAllowed);
                                pSidAllowed = NULL;
                                if (found) {
                                    p->isAllowedAccount = TRUE;
                                    break;
                                }
                            }
                        }
                    } else {
                        p->isAllowedAccount = FALSE;
                    }
                }
                if (p->isForbiddenAccount) {
                    reportPermissions = TRUE;
                    failed = TRUE;
                } else if (!(p->isAllowedAccount)) {
                    reportPermissions = TRUE;
                    failed |= wrapperData->secureFileCheckStrict;
                }
            }
            if (reportPermissions) {
                if (addToInsecureFileList(path, failed, permissions, FALSE)) {
                    reportPermissions = FALSE;
                }
            }
        }
    }
    if (!reportPermissions && permissions) {
        disposeSecureFilePermissionList(permissions);
    }

    /* Keep the result for next check on the same file. */
    hashMapPutKWVI(checkedFilesVarMap, path, failed ? FILE_INSECURE : FILE_SECURE);

    return failed;
}

/**
 * Print problematic permissions for the current file.
 *
 * @param permissions The permissions of the current file.
 * @param loglevel    LEVEL_NONE when querying pMaxNameLen, or the log level at which to print messages otherwise.
 * @param pMaxNameLen When loglevel is LEVEL_NONE, this pointer will be set to the longest account name length.
 */
static void printProblematicPermissions(SecureFilePermissionList* permissions, int loglevel, size_t* pMaxNameLen) {
    const TCHAR* message;
    size_t nameLen;
    SecureFilePermission* p;

    for (p = permissions->first; p; p = p->next) {
        message = NULL;
        if (p->isForbiddenAccount) {
            message = TEXT("This type of account is considered insecure and is not permitted.");
        } else if (!p->isAllowedAccount) {
            if (((p->mask & ACCESS_READ) == ACCESS_READ) && ((p->mask & ACCESS_WRITE) == ACCESS_WRITE)) {
                message = TEXT("Either allow this account in the configuration or remove the read and write file permissions.");
            } else if ((p->mask & ACCESS_WRITE) == ACCESS_WRITE) {
                message = TEXT("Either allow this account in the configuration or remove the write file permission.");
            } else if ((p->mask & ACCESS_READ) == ACCESS_READ) {
                message = TEXT("Either allow this account in the configuration or remove the read file permission.");
            }
        }

        if (message) {
            if (loglevel == LEVEL_NONE) {
                nameLen = _tcslen(p->accountName);
                if (nameLen > *pMaxNameLen) {
                    *pMaxNameLen = nameLen;
                }
            } else {
                log_printf(WRAPPER_SOURCE_WRAPPER, loglevel, TEXT("    %s%*s : %s"),
                    p->accountName, *pMaxNameLen - _tcslen(p->accountName), "", message);
            }
        }
    }
}

/**
 * Print the permission status of the current file for accounts allowed in the configuration.
 *
 * @param permissions The permissions of the current file.
 * @param loglevel    LEVEL_NONE when querying pMaxNameLen, or the log level at which to print messages otherwise.
 * @param pMaxNameLen When loglevel is LEVEL_NONE, this pointer will be set to the longest account name length.
 */
static void printAllowedPermissions(SecureFilePermissionList* permissions, int loglevel, size_t* pMaxNameLen) {
    const TCHAR* message;
    size_t nameLen;
    int i;
    PSID pSidAllowed = NULL;
    SecureFilePermission* p;

    for (i = 0; allowedAccountOrGroupNames[i]; i++) {
        for (p = permissions->first; p; p = p->next) {
            getTrusteeSidFromName(allowedAccountOrGroupNames[i], &pSidAllowed);
            if (pSidAllowed && EqualSid(p->pSid, pSidAllowed)) {
                break;
            }
        }

        if (!p) {
            message = TEXT("Allowed in the configuration but file permission not set.");
        } else if (((p->mask & ACCESS_READ) == ACCESS_READ) && ((p->mask & ACCESS_WRITE) == ACCESS_WRITE)) {
            message = TEXT("Allowed in the configuration and both read and write file permissions set.");
        } else if ((p->mask & ACCESS_WRITE) == ACCESS_WRITE) {
            message = TEXT("Allowed in the configuration and write file permission set.");
        } else if ((p->mask & ACCESS_READ) == ACCESS_READ) {
            message = TEXT("Allowed in the configuration and read file permission set.");
        } else {
            message = NULL;
        }
        if (message) {
            if (loglevel == LEVEL_NONE) {
                nameLen = _tcslen(allowedAccountOrGroupNames[i]);
                if (nameLen > *pMaxNameLen) {
                    *pMaxNameLen = nameLen;
                }
            } else {
                log_printf(WRAPPER_SOURCE_WRAPPER, loglevel, TEXT("    %s%*s : %s"),
                    allowedAccountOrGroupNames[i], *pMaxNameLen - _tcslen(allowedAccountOrGroupNames[i]), "", message);
            }
        }
    }
}
#else
static int isForbiddenGroup(const TCHAR* group) {
    return ((strcmpIgnoreCase(group, TEXT("users")) == 0) ||
            (strcmpIgnoreCase(group, TEXT("other")) == 0) ||
            (strcmpIgnoreCase(group, TEXT("everyone")) == 0) ||
            (strcmpIgnoreCase(group, TEXT("staff")) == 0));
}

static SecureFilePermission* addSecureFilePermission(SecureFilePermissionList* list, TCHAR* accountName, int class, id_t id, int mask) {
    SecureFilePermission* p;

    p = malloc(sizeof(SecureFilePermission));
    if (!p) {
        outOfMemory(TEXT("ASFP"), 1);
    } else {
        p->accountName = NULL;
        if (accountName) {
            updateStringValue(&p->accountName, accountName);
        }
        p->classShift = class;
        switch (class) {
        case SHIFT_OWNER:
            p->isAllowedAccount = TRUE;
            p->isForbiddenAccount = FALSE;
            break;

        case SHIFT_GROUP:
            p->isAllowedAccount = FALSE;    /* will be set to TRUE if present in the conf list */
            p->isForbiddenAccount = isForbiddenGroup(accountName);
            break;

        case SHIFT_OTHERS:
        default:
            p->isAllowedAccount = FALSE;
            p->isForbiddenAccount = TRUE;
            break;
        }
        p->id = id;
        p->mask = mask;
        p->next = NULL;

        if (list->last) {
            list->last->next = p;
        }
        list->last = p;
        if (!list->first) {
            list->first = p;
        }
    }

    return p;
}

#define OCTAL_PERM_OTH(mode)    (mode & (S_IROTH | S_IWOTH | S_IXOTH))
#define OCTAL_PERM_GRP(mode)   ((mode & (S_IRGRP | S_IWGRP | S_IXGRP)) >> 3)

#define SYMB_PERM_R(oct)       ((oct & 4) ? TEXT('r') : TEXT('-'))
#define SYMB_PERM_W(oct)       ((oct & 2) ? TEXT('w') : TEXT('-'))
#define SYMB_PERM_X(oct)       ((oct & 1) ? TEXT('x') : TEXT('-'))

int checkFilePermissions(const TCHAR* path) {
    struct stat fileStat;
    struct group *fileGr;
    struct group *confGr;
    const TCHAR* errorMessage = NULL;
    SecureFilePermissionList* permissions = NULL;
    SecureFilePermission* p;
    TCHAR* groupName = NULL;
    int reportPermissions = FALSE;
    int failed = FALSE;
    int i;

    switch (hashMapGetKWVI(checkedFilesVarMap, path)) {
    case FILE_INSECURE:
        return TRUE;

    case FILE_SECURE:
        return FALSE;

    case FILE_NOT_CHECKED:
    default:
        break;
    }

    /* First collect the permissions. If any error occurs, it will be reported below. */
    if (_tstat(path, &fileStat) < 0) {
        errorMessage = getLastErrorText();
        failed = TRUE;
 #ifdef CHECK_FILE_SYSTEM_SUPPORTED
    } else if ((fileStat.st_mode & S_IROTH) &&
               (fileStat.st_mode & S_IWOTH) &&
               (fileStat.st_mode & S_IXOTH) && !isSafeFileSystem(path)) {
        /* The permissions appear as '777' if the file system does not support Unix file permissions. */
 #endif
    } else {
        permissions = malloc(sizeof(SecureFilePermissionList));
        if (!permissions) {
            outOfMemory(TEXT("CFP"), 1);
            failed = TRUE;
        } else {
            memset(permissions, 0, sizeof(SecureFilePermissionList));

            if ((fileStat.st_mode & S_IROTH) ||
                (fileStat.st_mode & S_IWOTH) ||
                (fileStat.st_mode & S_IXOTH)) {
                /* 'others' bit is defined - this is never permitted */
                if (!addSecureFilePermission(permissions, NULL, SHIFT_OTHERS, 0, OCTAL_PERM_OTH(fileStat.st_mode))) {
                    /* OOM - already reported */
                    failed = TRUE;
                }
            }

            if ((fileStat.st_mode & S_IRGRP) ||
                (fileStat.st_mode & S_IWGRP) ||
                (fileStat.st_mode & S_IXGRP)) {
                /* 'group' bit is defined - the actual group must be explicitly allowed */
                fileGr = getgrgid(fileStat.st_gid);
                if (!fileGr) {
                    errorMessage = TEXT("Failed to retrieve information of the group.");
                    failed = TRUE;
                } else {
                    if (converterMBToWide(fileGr->gr_name, __UTF8, &groupName, FALSE)) {
                        updateStringValue(&groupName, TEXT("<unknown>"));
                    }
                    p = addSecureFilePermission(permissions, groupName, SHIFT_GROUP, fileGr->gr_gid, OCTAL_PERM_GRP(fileStat.st_mode));
                    if (!p) {
                        /* OOM - already reported */
                        failed = TRUE;
                    }
                    free(groupName);
                }
            }
        }
    }

    if (failed) {
        log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_FATAL, TEXT("Failed to check permissions of %s. %s"), path, errorMessage ? errorMessage : TEXT("Failed to retrieve existing permissions."));
    } else {
        /* Now, check if the permissions are allowed. */
        if (!permissions) {
            /* Unsecured volume */
            if (!allowUnsecuredVolumes) {
                failed |= wrapperData->secureFileCheckStrict;
                addToInsecureFileList(path, failed, NULL, TRUE);
            }
        } else {
            for (p = permissions->first; p; p = p->next) {
                /* There should be max 2 permissions: one for the group, and one for others. */
                if (p->classShift == SHIFT_GROUP) {
                    if (wrapperData->secureFileCheckDisabled) {
                        p->isAllowedAccount = TRUE;
                    } else {
                        for (i = 0; allowedAccountOrGroupNamesMb[i]; i++) {
                            confGr = getgrnam(allowedAccountOrGroupNamesMb[i]);
                            if (confGr && (confGr->gr_gid == p->id)) {
                                p->isAllowedAccount = TRUE;
                                break;
                            }
                        }
                    }
                }
                if (p->isForbiddenAccount) {
                    reportPermissions = TRUE;
                    failed |= TRUE;
                } else if (!(p->isAllowedAccount)) {
                    reportPermissions = TRUE;
                    failed |= wrapperData->secureFileCheckStrict;
                }
            }
            if (reportPermissions) {
                if (addToInsecureFileList(path, failed, permissions, FALSE)) {
                    reportPermissions = FALSE;
                }
            }
        }
    }
    if (!reportPermissions && permissions) {
        disposeSecureFilePermissionList(permissions);
    }

    /* Keep the result for next check on the same file. */
    hashMapPutKWVI(checkedFilesVarMap, path, failed ? FILE_INSECURE : FILE_SECURE);

    return failed;
}

/**
 * Print problematic permissions for the current file.
 *
 * @param permissions The permissions of the current file.
 * @param loglevel    The log level at which to print messages.
 * @param pMaxNameLen Not used, but keep for compatibility with Windows.
 */
static void printProblematicPermissions(SecureFilePermissionList* permissions, int loglevel, size_t* pMaxNameLen) {
    SecureFilePermission* p;

    for (p = permissions->first; p; p = p->next) {
        if (p->classShift == SHIFT_OTHERS) {
            log_printf(WRAPPER_SOURCE_WRAPPER, loglevel, TEXT("    Permission '0%d' ('%c%c%c') was granted to others. It must be '00'."),
                p->mask, SYMB_PERM_R(p->mask), SYMB_PERM_W(p->mask), SYMB_PERM_X(p->mask));
        } else if (p->isForbiddenAccount) {
            log_printf(WRAPPER_SOURCE_WRAPPER, loglevel, TEXT("    Permission '0%d0' ('%c%c%c') was granted to group '%s'. This group is considered insecure and is not permitted."),
                p->mask, SYMB_PERM_R(p->mask), SYMB_PERM_W(p->mask), SYMB_PERM_X(p->mask), p->accountName);
        } else if (!p->isAllowedAccount) {
            log_printf(WRAPPER_SOURCE_WRAPPER, loglevel, TEXT("    Permission '0%d0' ('%c%c%c') was granted to group '%s', but this group was not allowed in the configuration."),
                p->mask, SYMB_PERM_R(p->mask), SYMB_PERM_W(p->mask), SYMB_PERM_X(p->mask), p->accountName);
        }
    }
}

/**
 * Print the permission status of the current file for groups allowed in the configuration.
 *
 * @param permissions The permissions of the current file.
 * @param loglevel    LEVEL_NONE when querying pMaxNameLen, or the log level at which to print messages otherwise.
 * @param pMaxNameLen When loglevel is LEVEL_NONE, this pointer will be set to the longest group name length.
 */
static void printAllowedPermissions(SecureFilePermissionList* permissions, int loglevel, size_t* pMaxNameLen) {
    size_t nameLen;
    int calculateLen;
    int i;
    SecureFilePermission* p;
    struct group *confGr;

    /* find the permission for the group associated with the file */
    for (p = permissions->first; p; p = p->next) {
        if (p->classShift == SHIFT_GROUP) {
            break;
        }
    }

    for (i = 0; allowedAccountOrGroupNamesMb[i]; i++) {
        calculateLen = FALSE;
        if (!p) {
            if (loglevel == LEVEL_NONE) {
                calculateLen = TRUE;
            } else {
                log_printf(WRAPPER_SOURCE_WRAPPER, loglevel, TEXT("    %s%*s : This group is allowed in the configuration, but no group permissions are set for the file."),
                    allowedAccountOrGroupNames[i], *pMaxNameLen - _tcslen(allowedAccountOrGroupNames[i]), "");
            }
        } else {
            confGr = getgrnam(allowedAccountOrGroupNamesMb[i]);
            if (!confGr) {
                /* invalid group - be safe, but should not happen as we checked while loading */
            } else if (confGr->gr_gid != p->id) {
                if (loglevel == LEVEL_NONE) {
                    calculateLen = TRUE;
                } else {
                    log_printf(WRAPPER_SOURCE_WRAPPER, loglevel, TEXT("    %s%*s : This group is allowed in the configuration, but the group associated with the file is '%s'."),
                        allowedAccountOrGroupNames[i], *pMaxNameLen - _tcslen(allowedAccountOrGroupNames[i]), "", p->accountName);
                }
            } else {
                if (loglevel == LEVEL_NONE) {
                    calculateLen = TRUE;
                } else {
                    log_printf(WRAPPER_SOURCE_WRAPPER, loglevel, TEXT("    %s%*s : This group is allowed in the configuration and is granted file permissions '0%d0' ('%c%c%c')."),
                        allowedAccountOrGroupNames[i], *pMaxNameLen - _tcslen(allowedAccountOrGroupNames[i]), "", p->mask, SYMB_PERM_R(p->mask), SYMB_PERM_W(p->mask), SYMB_PERM_X(p->mask));
                }
            }
        }
        if (calculateLen) {
            nameLen = _tcslen(allowedAccountOrGroupNames[i]);
            if (nameLen > *pMaxNameLen) {
                *pMaxNameLen = nameLen;
            }
        }
    }
}
#endif

#define PRINT_EMPTY_LINE_IF_NOT_DEBUG(loglevel) \
        log_printf(WRAPPER_SOURCE_WRAPPER, loglevel == LEVEL_DEBUG ? LEVEL_NONE : loglevel, TEXT(""))

int checkAndReportInsecureFiles() {
    InsecureFile* insecureFile;
    int loglevel;
    size_t maxNameLen;
    int explainPermissions = FALSE;
    int result = FALSE;

    if (insecureFiles) {
        for (insecureFile = insecureFiles->first; insecureFile; insecureFile = insecureFile->next) {
            loglevel = insecureFile->critical ? LEVEL_FATAL : wrapperData->secureFileCheckLogLevel;
            if ((getLowLogLevel() <= loglevel) && (loglevel != LEVEL_NONE)) {
                if (insecureFile->isOnUnsecuredVolume) {
                    log_printf(WRAPPER_SOURCE_WRAPPER, loglevel, TEXT("The file '%s' is located on a volume whose format does not support file permissions. Anyone with access to the volume can read the file."), insecureFile->path);
#ifdef WIN32
                    log_printf(WRAPPER_SOURCE_WRAPPER, loglevel, TEXT("  Use '%s' or move the file to a different volume."), TEXT("wrapper.secure_file.check.windows.allow_unsecured_volumes=TRUE"));
#else
                    log_printf(WRAPPER_SOURCE_WRAPPER, loglevel, TEXT("  Use '%s' or move the file to a different volume."), TEXT("wrapper.secure_file.check.unix.allow_unsecured_volumes=TRUE"));
#endif
                } else {
                    /* First query the size of the longest account name to align them nicely. */
                    maxNameLen = 0;
#ifdef WIN32
                    printProblematicPermissions(insecureFile->permissions, LEVEL_NONE, &maxNameLen);
#endif
                    if (!wrapperData->secureFileCheckDisabled && allowedAccountOrGroupNames[0]) {
                        printAllowedPermissions(insecureFile->permissions, LEVEL_NONE, &maxNameLen);
                    }

                    /* Now actually print the permissions issues. */
                    log_printf(WRAPPER_SOURCE_WRAPPER, loglevel, TEXT("The file '%s' contains sensitive data but its permissions are too open."), insecureFile->path);
                    log_printf(WRAPPER_SOURCE_WRAPPER, loglevel, TEXT("  Problematic Permissions:"));
                    printProblematicPermissions(insecureFile->permissions, loglevel, &maxNameLen);
                    if (!wrapperData->secureFileCheckDisabled) {
                        if (allowedAccountOrGroupNames[0]) {
#ifdef WIN32
                            log_printf(WRAPPER_SOURCE_WRAPPER, loglevel, TEXT("  Allowed Permissions:"));
#else
                            log_printf(WRAPPER_SOURCE_WRAPPER, loglevel, TEXT("  Allowed Group Permissions:"));
#endif
                            printAllowedPermissions(insecureFile->permissions, loglevel, &maxNameLen);
                        }
                        explainPermissions = TRUE;
                    }
                }
                PRINT_EMPTY_LINE_IF_NOT_DEBUG(loglevel);
            }

            result = result || insecureFile->critical;
        }
        disposeInsecureFileList(insecureFiles);

        if (explainPermissions) {
            loglevel = result ? LEVEL_FATAL : wrapperData->secureFileCheckLogLevel;
#ifdef WIN32
            log_printf(WRAPPER_SOURCE_WRAPPER, loglevel, TEXT("  If you want to run the Wrapper as a Windows Service, please add '%s' to the list of allowed accounts."), wrapperData->ntServiceAccount ? wrapperData->ntServiceAccount : TEXT(".\\SYSTEM"));
            if (wrapperData->isConsole) {
                log_printf(WRAPPER_SOURCE_WRAPPER, loglevel, TEXT("  If you want to run the Wrapper as a Console Application, please add '%s\\%s' to the list of allowed accounts."), wrapperData->domainName, wrapperData->userName);
            }
            log_printf(WRAPPER_SOURCE_WRAPPER, loglevel, TEXT("  All other file permissions should normally be removed (use the Security tab of the file(s) Properties)."));
#else
            log_printf(WRAPPER_SOURCE_WRAPPER, loglevel, TEXT("  When the Wrapper is intended to be run by a single user, it is best to set the\n   ownership of secured files to that user and remove all other permissions.\n   To allow multiple users to access your secure files, you can grant read\n   permissions to a group that only a restricted list of users belong to,\n   but this group must be explicitly allowed in the Wrapper configuration."));
#endif
            PRINT_EMPTY_LINE_IF_NOT_DEBUG(loglevel);
        }
    }
    return result;
}

void disposeSecureFiles() {
    int i;

    if (allowedAccountOrGroupNames) {
        for (i = 0; allowedAccountOrGroupNames[i]; i++) {
            free(allowedAccountOrGroupNames[i]);
        }
        free(allowedAccountOrGroupNames);
        allowedAccountOrGroupNames = NULL;
    }
#ifndef WIN32
    if (allowedAccountOrGroupNamesMb) {
        for (i = 0; allowedAccountOrGroupNamesMb[i]; i++) {
            free(allowedAccountOrGroupNamesMb[i]);
        }
        free(allowedAccountOrGroupNamesMb);
        allowedAccountOrGroupNamesMb = NULL;
    }
#endif
    freeHashMap(checkedFilesVarMap);
    checkedFilesVarMap = NULL;
}

/**
 * This has to be called prior to loading the configuration (also on re-load).
 */
int resetSecureFileChecks() {
    freeHashMap(checkedFilesVarMap);
    checkedFilesVarMap = newHashMap(4);
    if (!checkedFilesVarMap) {
        return TRUE;
    }
    return FALSE;
}

/**
 * This is only called on pre-load.
 */
int loadSecureFileConfiguration() {
#ifdef WIN32
    const TCHAR* propNameHead = TEXT("wrapper.secure_file.check.windows.allowed_account.");
#else
    const TCHAR* propNameHead = TEXT("wrapper.secure_file.check.unix.allowed_group.");
    char* groupNameMb = NULL;
    int j;
#endif
    int i;
    TCHAR **propNames = NULL;
    TCHAR **propValues = NULL;
    long unsigned int *propIndices = NULL;

    wrapperData->secureFileCheckDisabled = getBooleanProperty(properties, TEXT("wrapper.secure_file.check.disable"), FALSE);
    if (!wrapperData->secureFileCheckDisabled) {
        wrapperData->secureFileCheckStrict = getBooleanProperty(properties, TEXT("wrapper.secure_file.check.strict"), TRUE);
        wrapperData->secureFileCheckLogLevel = getLogLevelForName(getStringProperty(properties, TEXT("wrapper.secure_file.check.loglevel"), wrapperData->secureFileCheckStrict ? TEXT("FATAL") : TEXT("WARN")));

        if (getStringProperties(properties, propNameHead, TEXT(""), wrapperData->ignoreSequenceGaps, FALSE, &propNames, &propValues, &propIndices) == -1) {
            return TRUE;
        }

        /* The values need to be copied so we don't loose them when the configuration is reloaded. */
        for (i = 0; propValues[i]; i++);

        allowedAccountOrGroupNames = malloc(sizeof(TCHAR*) * (i + 1));
        if (!allowedAccountOrGroupNames) {
            outOfMemory(TEXT("LSFC"), 1);
            freeStringProperties(propNames, propValues, propIndices);
            return TRUE;
        }
        memset(allowedAccountOrGroupNames, 0, sizeof(TCHAR*) * (i + 1));
#ifdef WIN32
        allowUnsecuredVolumes = getBooleanProperty(properties, TEXT("wrapper.secure_file.check.windows.allow_unsecured_volumes"), FALSE);

        for (i = 0; propValues[i]; i++) {
            updateStringValue(&allowedAccountOrGroupNames[i], propValues[i]);
        }
#else
 #ifdef CHECK_FILE_SYSTEM_SUPPORTED
        allowUnsecuredVolumes = getBooleanProperty(properties, TEXT("wrapper.secure_file.check.unix.allow_unsecured_volumes"), FALSE);
 #else
        allowUnsecuredVolumes = FALSE;
 #endif

        allowedAccountOrGroupNamesMb = malloc(sizeof(char*) * (i + 1));
        if (!allowedAccountOrGroupNamesMb) {
            outOfMemory(TEXT("LSFC"), 2);
            freeStringProperties(propNames, propValues, propIndices);
            return TRUE;
        }
        memset(allowedAccountOrGroupNamesMb, 0, sizeof(char*) * (i + 1));
        for (i = 0, j = 0; propValues[i]; i++, j++) {
            /* Group names need to be converted in multibytes to be used by getgrnam(). */
            if (converterWideToMB(propValues[i], &groupNameMb, __UTF8) < 0) {
                if (groupNameMb) {
                    log_printf(WRAPPER_SOURCE_WRAPPER, LEVEL_ERROR,
                        TEXT("Encountered an invalid value for configuration property %s=%s.  Ignoring."), propNames[i], propValues[i]);
                    free(groupNameMb);
                    j--;
                } else {
                    outOfMemory(TEXT("LSFC"), 3);
                    freeStringProperties(propNames, propValues, propIndices);
                    return TRUE;
                }
            } else if (getgrnam(groupNameMb)) {
                updateStringValue(&allowedAccountOrGroupNames[j], propValues[i]);
                allowedAccountOrGroupNamesMb[j] = groupNameMb;
            } else {
                /* This group doesn't exist on the current system. Skip. */
                free(groupNameMb);
                j--;
            }
            groupNameMb = NULL;
        }
#endif
        freeStringProperties(propNames, propValues, propIndices);
    }

    return FALSE;
}
