/*
 * Fallback wrapperinfo.c used when building outside Ant.
 * Populates version/arch strings so native targets link without the Ant token
 * replacement step that normally generates this file from wrapperinfo.c.in.
 */

#ifdef WIN32
#include <tchar.h>
#endif

#include "wrapper_i18n.h"

/* Values below are conservative defaults for local builds. */
/* Define wide/ansi string constants directly to avoid macro surprises. */
#if defined(UNICODE) || defined(_UNICODE)
  #if defined(_M_ARM64) || defined(__aarch64__)
    const wchar_t wrapperBits[]  = L"64";
    const wchar_t wrapperArch[]  = L"arm64";
  #elif defined(_WIN64)
    const wchar_t wrapperBits[]  = L"64";
    const wchar_t wrapperArch[]  = L"x86_64";
  #else
    const wchar_t wrapperBits[]  = L"32";
    const wchar_t wrapperArch[]  = L"x86";
  #endif
  /* Version (wide): allow CI override via WRAPPER_VERSION_* macros. */
  #ifdef WRAPPER_VERSION_ROOT_W
    const wchar_t wrapperVersionRoot[]    = WRAPPER_VERSION_ROOT_W;
  #else
    const wchar_t wrapperVersionRoot[]    = L"3.6.2";
  #endif
  #ifdef WRAPPER_VERSION_W
    const wchar_t wrapperVersion[]        = WRAPPER_VERSION_W;
  #else
    const wchar_t wrapperVersion[]        = L"3.6.2";
  #endif
  const wchar_t wrapperOS[]               = L"windows";
  const wchar_t wrapperReleaseDate[]      = L"00000000";
  const wchar_t wrapperReleaseTime[]      = L"0000";
  /* Make wide strings from __DATE__/__TIME__ via literal concatenation. */
  const wchar_t wrapperBuildDate[]        = L"" __DATE__;
  const wchar_t wrapperBuildTime[]        = L"" __TIME__;
  const wchar_t wrapperJavacTargetVersion[] = L"unknown";
#else
  #if defined(_M_ARM64) || defined(__aarch64__)
    const char wrapperBits[]  = "64";
    const char wrapperArch[]  = "arm64";
  #elif defined(_WIN64)
    const char wrapperBits[]  = "64";
    const char wrapperArch[]  = "x86_64";
  #else
    const char wrapperBits[]  = "32";
    const char wrapperArch[]  = "x86";
  #endif
  /* Version (narrow): allow CI override via WRAPPER_VERSION_* macros. */
  #ifdef WRAPPER_VERSION_ROOT_A
    const char wrapperVersionRoot[]    = WRAPPER_VERSION_ROOT_A;
  #else
    const char wrapperVersionRoot[]    = "3.6.2";
  #endif
  #ifdef WRAPPER_VERSION_A
    const char wrapperVersion[]        = WRAPPER_VERSION_A;
  #else
    const char wrapperVersion[]        = "3.6.2";
  #endif
  const char wrapperOS[]               = "windows";
  const char wrapperReleaseDate[]      = "00000000";
  const char wrapperReleaseTime[]      = "0000";
  const char wrapperBuildDate[]        = __DATE__;
  const char wrapperBuildTime[]        = __TIME__;
  const char wrapperJavacTargetVersion[] = "unknown";
#endif
