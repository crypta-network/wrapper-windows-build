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

#ifndef WIN32
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <grp.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#ifdef LINUX
 #include <dirent.h>
 #include <arpa/inet.h>
 #include <ctype.h>
#endif
#include <unistd.h>
#include "loggerjni.h"
#include "wrapperjni.h"

pid_t wrapperProcessId = -1;
pthread_mutex_t controlEventQueueMutex = PTHREAD_MUTEX_INITIALIZER;


int wrapperLockControlEventQueue() {
    int count = 0;
    /* Only wait for up to 30 seconds to make sure we don't get into a deadlock situation.
     *  This could happen if a signal is encountered while locked. */
    while (pthread_mutex_trylock(&controlEventQueueMutex) == EBUSY) {
        if (count >= 3000) {
            log_printf(TEXT("WrapperJNI Error: Timed out waiting for internal lock (%s)."), TEXT("control event queue"));
            return -1;
        }
        wrapperSleep(10);
        count++;
    }

    if (count > 0) {
        if (wrapperJNIDebugging) {
            /* This is useful for making sure that the JNI call is working. */
            log_printf(TEXT("WrapperJNI Debug: Looped %d times before lock (%s)."), count, TEXT("control event queue"));
        }
    }
    return 0;
}

int wrapperReleaseControlEventQueue() {
    int ret = pthread_mutex_unlock(&controlEventQueueMutex);
    if (ret != 0) {
        log_printf(TEXT("WrapperJNI Error: Failed to release internal lock (%s, 0x%x)."), TEXT("control event queue"), ret);
    }
    return ret;
}

/**
 * Handle interrupt signals (i.e. Crtl-C).
 */
void handleInterrupt(int sig_num) {
    wrapperJNIHandleSignal(org_tanukisoftware_wrapper_WrapperManager_WRAPPER_CTRL_C_EVENT);
    signal(SIGINT, handleInterrupt);
}

/**
 * Handle termination signals (i.e. machine is shutting down).
 */
void handleTermination(int sig_num) {
    wrapperJNIHandleSignal(org_tanukisoftware_wrapper_WrapperManager_WRAPPER_CTRL_TERM_EVENT);
    signal(SIGTERM, handleTermination);
}

/**
 * Handle hangup signals.
 */
void handleHangup(int sig_num) {
    wrapperJNIHandleSignal(org_tanukisoftware_wrapper_WrapperManager_WRAPPER_CTRL_HUP_EVENT);
    signal(SIGHUP, handleHangup);
}

/**
 * Handle usr1 signals.
 *
 * SIGUSR1 & SIGUSR2 are used by the JVM for internal garbage collection sweeps.
 *  These signals MUST be passed on to the JVM or the JVM will hang.
 */
/*
 void handleUsr1(int sig_num) {
    wrapperJNIHandleSignal(org_tanukisoftware_wrapper_WrapperManager_WRAPPER_CTRL_USR1_EVENT);
    signal(SIGUSR1, handleUsr1);
}
 */

/**
 * Handle usr2 signals.
 *
 * SIGUSR1 & SIGUSR2 are used by the JVM for internal garbage collection sweeps.
 *  These signals MUST be passed on to the JVM or the JVM will hang.
 */
/*
 void handleUsr2(int sig_num) {
    wrapperJNIHandleSignal(org_tanukisoftware_wrapper_WrapperManager_WRAPPER_CTRL_USR2_EVENT);
    signal(SIGUSR2, handleUsr2);
}
*/

/*
 * Class:     org_tanukisoftware_wrapper_WrapperManager
 * Method:    nativeInit
 * Signature: (Z)V
 */
JNIEXPORT void JNICALL
Java_org_tanukisoftware_wrapper_WrapperManager_nativeInit(JNIEnv *env, jclass jClassWrapperManager, jboolean debugging) {
    TCHAR *retLocale;
    wrapperJNIDebugging = debugging;
#ifdef FREEBSD
    if (loadIconvLibrary()) {
        /* Already reported. */
        return;
    }
#endif

    /* Set the locale so we can display MultiByte characters. */
    retLocale = _tsetlocale(LC_ALL, TEXT(""));
#if defined(UNICODE)
    if (retLocale) {
        free(retLocale);
    }
#endif
    initLog(env);

    if (wrapperJNIDebugging) {
        /* This is useful for making sure that the JNI call is working. */
        log_printf(TEXT("WrapperJNI Debug: Inside native WrapperManager initialization method"));
    }

    /* Set handlers for signals */
    signal(SIGINT,  handleInterrupt);
    signal(SIGTERM, handleTermination);
    signal(SIGHUP,  handleHangup);
    /*
    signal(SIGUSR1, handleUsr1);
    signal(SIGUSR2, handleUsr2);
    */

    if (initCommon(env, jClassWrapperManager)) {
        /* Failed.  An exception will have been thrown. */
        return;
    }

    /* Store the current process Id */
    wrapperProcessId = getpid();
}

/*
 * Class:     org_tanukisoftware_wrapper_WrapperManager
 * Method:    nativeLoadWrapperProperties
 * Signature: ()V
 */
JNIEXPORT void JNICALL
Java_org_tanukisoftware_wrapper_WrapperManager_nativeLoadWrapperProperties(JNIEnv *env, jclass clazz) {
}

/*
 * Class:     org_tanukisoftware_wrapper_WrapperManager
 * Method:    nativeRaiseExceptionInner
 * Signature: (I)V
 */
JNIEXPORT void JNICALL
Java_org_tanukisoftware_wrapper_WrapperManager_nativeRaiseExceptionInner(JNIEnv *env, jclass clazz, jint code) {
    log_printf(TEXT("WrapperJNI: Not Windows.  RaiseException ignored."));
}

/*
 * Class:     org_tanukisoftware_wrapper_WrapperManager
 * Method:    nativeRaiseFailFastExceptionInner
 * Signature: ()V
 */
JNIEXPORT void JNICALL
Java_org_tanukisoftware_wrapper_WrapperManager_nativeRaiseFailFastExceptionInner(JNIEnv *env, jclass clazz) {
    log_printf(TEXT("WrapperJNI: Not Windows.  FailFastException ignored."));
}

/*
 * Class:     org_tanukisoftware_wrapper_WrapperManager
 * Method:    nativeRedirectPipes
 * Signature: ()I
 */
JNIEXPORT jint JNICALL
Java_org_tanukisoftware_wrapper_WrapperManager_nativeRedirectPipes(JNIEnv *env, jclass clazz) {
    int fd;
    
    fd = _topen(TEXT("/dev/null"), O_RDWR, 0);
    if (fd != -1) {
        if (!redirectedStdErr) {
            _ftprintf(stderr, TEXT("WrapperJNI: Redirecting %s to /dev/null\n"), TEXT("StdErr")); fflush(NULL);
            if (dup2(fd, STDERR_FILENO) == -1) {
                _ftprintf(stderr, TEXT("WrapperJNI: Failed to redirect %s to /dev/null  (Err: %s)\n"), TEXT("StdErr"), getLastErrorText()); fflush(NULL);
            } else {
                redirectedStdErr = TRUE;
            }
        }
        
        if (!redirectedStdOut) {
            log_printf(TEXT("WrapperJNI: Redirecting %s to /dev/null"), TEXT("StdOut"));
            if (dup2(fd, STDOUT_FILENO) == -1) {
                log_printf(TEXT("WrapperJNI: Failed to redirect %s to /dev/null  (Err: %s)"), TEXT("StdOut"), getLastErrorText());
            } else {
                redirectedStdOut = TRUE;
            }
        }
    } else {
        _ftprintf(stderr, TEXT("WrapperJNI: Failed to open /dev/null  (Err: %s)\n"), getLastErrorText()); fflush(NULL);
    }
    
    return 0;
}

/*
 * Class:     org_tanukisoftware_wrapper_WrapperManager
 * Method:    nativeConnectBackendPipeDescriptor
 * Signature: (I)Ljava/io/FileDescriptor;
 */
JNIEXPORT jobject JNICALL
Java_org_tanukisoftware_wrapper_WrapperManager_nativeGetFileDescriptor(JNIEnv *env, jclass clazz, jint jintFd) {
    jclass fdClass;
    jfieldID fdFieldID;
    jmethodID fdConstructor;
    jobject fileDesc = NULL;

    /* Note: If any of the steps below fail, an exception will be thrown, so the function should never return NULL. */

    /* Get the FileDescriptor class. */
    if ((fdClass = (*env)->FindClass(env, utf8javaioFileDescriptor)) != NULL) {
        /* Get the private "fd" field. */
        if ((fdFieldID = (*env)->GetFieldID(env, fdClass, utf8fd, utf8SigI)) != NULL) {
            /* Get the constructor. */
            if ((fdConstructor = (*env)->GetMethodID(env, fdClass, utf8MethodInit, utf8VrV)) != NULL) {
                /* Create a FileDescriptor instance (this is done last, so we don't need to release the instance if other steps fail). */
                if ((fileDesc = (*env)->NewObject(env, fdClass, fdConstructor)) != NULL) {
                    /* Set "fd" with the file descriptor number (jintFd). */
                    (*env)->SetIntField(env, fileDesc, fdFieldID, jintFd);
                }
            }
        }
        (*env)->DeleteLocalRef(env, fdClass);
    }
    return fileDesc;
}

/*
 * Class:     org_tanukisoftware_wrapper_WrapperManager
 * Method:    nativeGetJavaPID
 * Signature: ()I
 */
JNIEXPORT jint JNICALL
Java_org_tanukisoftware_wrapper_WrapperManager_nativeGetJavaPID(JNIEnv *env,
        jclass clazz) {
    return (int) getpid();
}

/*
 * Class:     org_tanukisoftware_wrapper_WrapperManager
 * Method:    nativeRequestThreadGroup
 * Signature: ()V
 */
JNIEXPORT void JNICALL
Java_org_tanukisoftware_wrapper_WrapperManager_nativeRequestThreadDump(
        JNIEnv *env, jclass clazz) {
    if (wrapperJNIDebugging) {
        log_printf(TEXT("WrapperJNI Debug: Sending SIGQUIT event to process group %d."),
            (int)wrapperProcessId);
    }
    if (kill(wrapperProcessId, SIGQUIT) < 0) {
        log_printf(TEXT("WrapperJNI Error: Unable to send SIGQUIT to JVM process: %s"),
            getLastErrorText());
    }
}

/*
 * Class:     org_tanukisoftware_wrapper_WrapperManager
 * Method:    nativeSetConsoleTitle
 * Signature: (Ljava/lang/String;)V
 */
JNIEXPORT void JNICALL
Java_org_tanukisoftware_wrapper_WrapperManager_nativeSetConsoleTitle(JNIEnv *env, jclass clazz, jstring jstringTitle) {
    if (wrapperJNIDebugging) {
        log_printf(TEXT("WrapperJNI Debug: Setting the console title not supported on UNIX platforms."));
    }
}

/*
 * Class:     org_tanukisoftware_wrapper_WrapperManager
 * Method:    nativeGetUser
 * Signature: (Z)Lorg/tanukisoftware/wrapper/WrapperUser;
 */
/*#define UVERBOSE*/
JNIEXPORT jobject JNICALL
Java_org_tanukisoftware_wrapper_WrapperManager_nativeGetUser(JNIEnv *env, jclass clazz, jboolean groups) {
    jclass wrapperUserClass;
    jmethodID constructor;
    jmethodID setGroup;
    jmethodID addGroup;
    uid_t uid;
    struct passwd *pw;
    gid_t ugid;
    jstring jstringUser;
    jstring jstringRealName;
    jstring jstringHome;
    jstring jstringShell;
    jobject wrapperUser = NULL;
    struct group *aGroup;
    int member;
    int i;
    gid_t ggid;
    jstring jstringGroupName;

    /* Look for the WrapperUser class. Ignore failures as JNI throws an exception. */
    if ((wrapperUserClass = (*env)->FindClass(env, utf8ClassOrgTanukisoftwareWrapperWrapperUNIXUser)) != NULL) {

        /* Look for the constructor. Ignore failures. */
        if ((constructor = (*env)->GetMethodID(env, wrapperUserClass, utf8MethodInit, utf8SigIIStringStringStringStringrV)) != NULL) {

            uid = geteuid();
            pw = getpwuid(uid);
            ugid = pw->pw_gid;

            /* Create the arguments to the constructor as java objects */
            /* User */
            jstringUser = JNU_NewStringFromNativeMB(env, pw->pw_name);
            if (jstringUser) {
                /* Real Name */
                jstringRealName = JNU_NewStringFromNativeMB(env, pw->pw_gecos);
                if (jstringRealName) {
                    /* Home */
                    jstringHome = JNU_NewStringFromNativeMB(env, pw->pw_dir);
                    if (jstringHome) {
                        /* Shell */
                        jstringShell = JNU_NewStringFromNativeMB(env, pw->pw_shell);
                        if (jstringShell) {
                            /* Now create the new wrapperUser using the constructor arguments collected above. */
                            wrapperUser = (*env)->NewObject(env, wrapperUserClass, constructor,
                                    uid, ugid, jstringUser, jstringRealName, jstringHome, jstringShell);

                            /* If the caller requested the user's groups then look them up. */
                            if (groups) {
                                /* Set the user group. */
                                if ((setGroup = (*env)->GetMethodID(env, wrapperUserClass, utf8MethodSetGroup, utf8SigIStringrV)) != NULL) {
                                    if ((aGroup = getgrgid(ugid)) != NULL) {
                                        ggid = aGroup->gr_gid;

                                        /* Group name */
                                        jstringGroupName = JNU_NewStringFromNativeMB(env, aGroup->gr_name);
                                        if (jstringGroupName) {
                                            /* Add the group to the user. */
                                            (*env)->CallVoidMethod(env, wrapperUser, setGroup,
                                                    ggid, jstringGroupName);

                                            (*env)->DeleteLocalRef(env, jstringGroupName);
                                        } else {
                                            /* Exception Thrown */
                                        }
                                    }
                                } else {
                                    /* Exception Thrown */
                                }

                                /* Look for the addGroup method. Ignore failures. */
                                if ((addGroup = (*env)->GetMethodID(env, wrapperUserClass, utf8MethodAddGroup, utf8SigIStringrV)) != NULL) {
                                    setgrent();
                                    while ((aGroup = getgrent()) != NULL) {
                                        /* Search the member list to decide whether or not the user is a member. */
                                        member = 0;
                                        i = 0;
                                        while ((member == 0) && aGroup->gr_mem[i]) {
                                            if (strcmp(aGroup->gr_mem[i], pw->pw_name) == 0) {
                                               member = 1;
                                            }
                                            i++;
                                        }

                                        if (member) {
                                            ggid = aGroup->gr_gid;

                                            /* Group name */
                                            jstringGroupName = JNU_NewStringFromNativeMB(env, aGroup->gr_name);
                                            if (jstringGroupName) {
                                                /* Add the group to the user. */
                                                (*env)->CallVoidMethod(env, wrapperUser, addGroup,
                                                        ggid, jstringGroupName);

                                                (*env)->DeleteLocalRef(env, jstringGroupName);
                                            } else {
                                                /* Exception Thrown */
                                            }
                                        }
                                    }
                                    endgrent();
                                } else {
                                    /* Exception Thrown */
                                }
                            }

                            (*env)->DeleteLocalRef(env, jstringShell);
                        } else {
                            /* Exception Thrown */
                        }

                        (*env)->DeleteLocalRef(env, jstringHome);
                    } else {
                        /* Exception Thrown */
                    }

                    (*env)->DeleteLocalRef(env, jstringRealName);
                } else {
                    /* Exception Thrown */
                }

                (*env)->DeleteLocalRef(env, jstringUser);
            } else {
                /* Exception Thrown */
            }
        } else {
            /* Exception Thrown */
        }

        (*env)->DeleteLocalRef(env, wrapperUserClass);
    }

    return wrapperUser;
}

/*
 * Class:     org_tanukisoftware_wrapper_WrapperManager
 * Method:    nativeGetInteractiveUser
 * Signature: (Z)Lorg/tanukisoftware/wrapper/WrapperUser;
 */
JNIEXPORT jobject JNICALL
Java_org_tanukisoftware_wrapper_WrapperManager_nativeGetInteractiveUser(JNIEnv *env, jclass clazz, jboolean groups) {
    /* If the DISPLAY environment variable is set then assume that this user
     *  has access to an X display, in which case we will return the same thing
     *  as nativeGetUser. */
    if (getenv("DISPLAY")) {
        /* This is an interactive JVM since it has access to a display. */
        return Java_org_tanukisoftware_wrapper_WrapperManager_nativeGetUser(env, clazz, groups);
    } else {
        /* There is no DISPLAY variable, so assume that this JVM is non-interactive. */
        return NULL;
    }
}

/*
 * Class:     org_tanukisoftware_wrapper_WrapperManager
 * Method:    nativeListServices
 * Signature: ()[Lorg/tanukisoftware/wrapper/WrapperWin32Service;
 */
JNIEXPORT jobjectArray JNICALL
Java_org_tanukisoftware_wrapper_WrapperManager_nativeListServices(JNIEnv *env, jclass clazz) {
    /** Not supported on UNIX platforms. */
    return NULL;
}

/*
 * Class:     org_tanukisoftware_wrapper_WrapperManager
 * Method:    nativeSendServiceControlCode
 * Signature: (Ljava/lang/String;I)Lorg/tanukisoftware/wrapper/WrapperWin32Service;
 */
JNIEXPORT jobject JNICALL
Java_org_tanukisoftware_wrapper_WrapperManager_nativeSendServiceControlCode(JNIEnv *env, jclass clazz, jbyteArray serviceName, jint controlCode) {
    /** Not supported on UNIX platforms. */
    return NULL;
}

/*
 * Class:     org_tanukisoftware_wrapper_WrapperManager
 * Method:    nativeGetPortStatus
 * Signature: (ILjava/lang/String;I)I
 *
 * @param port The port whose status is requested.
 * @param jAddress IPv4 or IPv6 address.
 * @param protocol The protocol of the port, 0=tcpv4, 1=tcpv6
 *
 * @return The status, -1=error, 0=closed, >0=in use.
 */
JNIEXPORT jint JNICALL
Java_org_tanukisoftware_wrapper_WrapperManager_nativeGetPortStatus(JNIEnv *env, jclass clazz, jint port, jstring jAddress, jint protocol) {
    /* Not implemented on UNIX as it is not needed for now. May implement if ever made publicly available. */
    return 0;
}

#ifdef LINUX
/* Convert a string IPv4 address to a 32-bit integer. */
static unsigned long ipv4ToUlong(const char *ip_str) {
    struct in_addr addr;
    if (inet_pton(AF_INET, ip_str, &addr) != 1) {
        return 0;
    }
    return addr.s_addr;
}

 #ifdef IPV6_SUPPORT
/* Convert a string IPv6 address to a byte array. */
static int fillIpv6ByteArray(unsigned char *buffer, const char *ip_str) {
    struct in6_addr addr;
    if (inet_pton(AF_INET6, ip_str, &addr) != 1) {
        return -1;
    }
    memcpy(buffer, &addr, sizeof(addr));
    return 0;
}
 #endif

static int hasSocketDescriptor(pid_t pid, unsigned long inode) {
    char buffer1[32];
    char buffer2[256];
    char *ptr = NULL;
    size_t len1, len2;
    DIR *fdDir;
    struct dirent *entry;
    unsigned long tempInode;
    int result = FALSE;

    snprintf(buffer1, sizeof(buffer1), "/proc/%d/fd", pid);
    fdDir = opendir(buffer1);
    if (fdDir) {
        len1 = strlen(buffer1);
        if (len1 < sizeof(buffer1) - 2) {
            buffer1[len1] = '/';
            while ((entry = readdir(fdDir)) != NULL) {
                if (entry->d_type == DT_LNK) {
                    snprintf(buffer1 + len1 + 1, sizeof(buffer1) - len1 - 1, "%.*s", (int)(sizeof(buffer1) - len1 - 1 - 1), entry->d_name);

                    /* Read the symlink to get the info of the file descriptor. */
                    len2 = readlink(buffer1, buffer2, sizeof(buffer2) - 1);
                    if (len2 != -1) {
                        buffer2[len2] = 0;
                        if ((ptr = strstr(buffer2, "socket:[")) != NULL) {
                            sscanf(ptr + 8, "%lx", &tempInode);
                            if (tempInode == inode) {
                                result = TRUE;
                                break;
                            }
                        }
                    }
                }
            }
        }
        closedir(fdDir);
    }
    return result;
}
#endif

/*
 * Class:     org_tanukisoftware_wrapper_WrapperManager
 * Method:    nativeGetSocketRemotePID
 * Signature: (ILjava/lang/String;I)I
 *
 * @param port The remote port.
 * @param jAddress IPv4 or IPv6 address.
 * @param protocol The protocol of the port, 0=tcpv4, 1=tcpv6
 * @param state The state, 0=listening, 1=established
 *
 * @return The pid of the remote process.
 */
JNIEXPORT jint JNICALL
Java_org_tanukisoftware_wrapper_WrapperManager_nativeGetSocketRemotePID(JNIEnv *env, jclass clazz, jint port, jstring jAddress, jint protocol, jint state) {
    pid_t pid = 0;
#ifdef LINUX
    pid_t tempPid;
    const char *nativeAddress;
    unsigned long local_addr_ipv4 = 0;
 #ifdef IPV6_SUPPORT
    int ret;
    unsigned char local_addr_ipv6[16] = {0};
 #endif
    int tcpState;
    char buffer[256];
    char *ptr1 = NULL;
    char *ptr2 = NULL;
    char *ptr3 = NULL;
    TCHAR* fileName; 
    FILE *file = NULL;
    unsigned long local_addr;
    unsigned int local_port;
    unsigned int st = 0;
    unsigned long inode = 0;
    TCHAR* wrapperPidStr;
    pid_t wrapperPid = 0;
    DIR *procDir;
    struct dirent *entry;

    /* 1) Convert the address to the appropriate format (ipv4 uses ulong, ipv6 uses bytes array). */
    nativeAddress = (*env)->GetStringUTFChars(env, jAddress, 0);
    if (!nativeAddress) {
        throwOutOfMemoryError(env, TEXT("GSRP2"));
        return 0;
    }
    if (protocol == 0) {
        /* IPv4 */
        local_addr_ipv4 = ipv4ToUlong(nativeAddress);
        (*env)->ReleaseStringUTFChars(env, jAddress, nativeAddress);
        if (local_addr_ipv4 == 0) {
            /* Failed. */
            return 0;
        }
        fileName = TEXT("/proc/net/tcp");
    } else {
        /* IPv6 */
 #ifdef IPV6_SUPPORT
        ret = fillIpv6ByteArray(local_addr_ipv6, nativeAddress);
        (*env)->ReleaseStringUTFChars(env, jAddress, nativeAddress);
        if (ret == 0) {
            /* Failed. */
            return 0;
        }
        fileName = TEXT("/proc/net/tcp6");
 #else
        return 0;
 #endif
    }

    /* 2) Figure out which state to select. */
    switch (state) {
    case 0:
        tcpState = 10; /* TCP_LISTEN */
        break;

    case 1:
        tcpState = 1; /* TCP_ESTABLISHED; */
        break;

    default:
        /* Invalid. */
        return 0;
    }

    /* 3) Seek for our socket in the tcp file and retrieve the inode. */
    file = _tfopen(fileName, TEXT("r"));
    if (!file) {
        return 0;
    }
    if (fgets(buffer, sizeof(buffer), file)) {
        /* First line is the header: '  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode' */
        ptr1 = strstr(buffer, "local_address");
        ptr2 = strstr(buffer, "st ");
        ptr3 = strstr(buffer, "inode");
    }
    if (ptr1 && ptr2 && ptr3) {
        while (fgets(buffer, sizeof(buffer), file)) {
 #ifdef IPV6_SUPPORT
            /* Not implemented */
            break;
 #else
            /* address:port */
            if (sscanf(ptr1, "%lx:%x", &local_addr, &local_port) == 2) {
                if ((local_addr == local_addr_ipv4) && (local_port == port)) {
                    /* status (listening=0A, established=01) - IMPORTANT: there might be a delay for /proc/net/tcp to be updated! */
                    if (sscanf(ptr2, "%x", &st) == 1) {
                        if (st == tcpState) {
                            /* inode */
                            sscanf(ptr3, "%lx", &inode);
                            break;
                        }
                    }
                }
            }
 #endif
        }
    }
    fclose(file);
    if (inode == 0) {
        return 0;
    }

    /* 4) Start by checking if the Wrapper owns the socket, as this is the most likely case. */
    if (!getSystemProperty(env, TEXT("wrapper.pid"), &wrapperPidStr, FALSE) && wrapperPidStr) {
        wrapperPid = _ttoi(wrapperPidStr);
        if ((wrapperPid > 0) && hasSocketDescriptor(wrapperPid, inode)) {
            return wrapperPid;
        }
    }

    /* 5) Search for other processes: Iterate through the /proc folders and try to find the corresponding inode. */
    procDir = opendir("/proc");
    if (!procDir) {
        return 0;
    }
    while ((entry = readdir(procDir)) != NULL) {
        if ((entry->d_type == DT_DIR) && isdigit(entry->d_name[0])) {   /* look into directories whose name are PIDs */
            tempPid = atoi(entry->d_name);
            if ((tempPid != wrapperPid) && (tempPid != getpid())) {  /* make sure to not look into the folder of the current process */
                if (hasSocketDescriptor(tempPid, inode)) {
                    pid = tempPid;
                    break;
                }
            }
        }
    }
    closedir(procDir);
#else
    /* /proc can't be used on other Unix platforms.
     * It is possible to retrieve the socket owner's PID using getsockopt() with SO_PEERCRED, but this only works with Unix domain sockets (AF_UNIX). */
#endif
    return pid;
}

/*
 * Class:     org_tanukisoftware_wrapper_WrapperManager
 * Method:    nativeSetDpiAwareness
 * Signature: (I)V
 *
 * @param awareness The DPI awareness value to set.
 */
JNIEXPORT void JNICALL
Java_org_tanukisoftware_wrapper_WrapperManager_nativeSetDpiAwareness(JNIEnv *env, jclass clazz, jint awareness) {
    /* Not implemented on UNIX. */
    return;
}

/*
 * Class:     org_tanukisoftware_wrapper_WrapperManager
 * Method:    nativeGetDpiAwareness
 * Signature: ()I
 *
 * @return The dpi Awareness of the current process, or -1 if there was an error.
 */
JNIEXPORT jint JNICALL
Java_org_tanukisoftware_wrapper_WrapperManager_nativeGetDpiAwareness(JNIEnv *env, jclass clazz) {
    /* Not implemented on UNIX. */
    return -1;
}

/*
 * Class:     org_tanukisoftware_wrapper_WrapperManager
 * Method:    nativeGetDpiScale
 * Signature: ()I
 *
 * @return The dpi scale (should be devided by 96 to get the scale factor).
 */
JNIEXPORT jint JNICALL
Java_org_tanukisoftware_wrapper_WrapperManager_nativeGetDpiScale(JNIEnv *env, jclass clazz) {
    /* Not implemented on UNIX: always return 96 as it is the default value. */
    return 96;
}
#endif
