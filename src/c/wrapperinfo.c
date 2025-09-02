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
  #ifdef _WIN64
    const wchar_t wrapperBits[]  = L"64";
    const wchar_t wrapperArch[]  = L"x86_64";
  #else
    const wchar_t wrapperBits[]  = L"32";
    const wchar_t wrapperArch[]  = L"x86";
  #endif
  const wchar_t wrapperVersionRoot[]      = L"dev";
  const wchar_t wrapperVersion[]          = L"dev";
  const wchar_t wrapperOS[]               = L"windows";
  const wchar_t wrapperReleaseDate[]      = L"00000000";
  const wchar_t wrapperReleaseTime[]      = L"0000";
  /* Make wide strings from __DATE__/__TIME__ via literal concatenation. */
  const wchar_t wrapperBuildDate[]        = L"" __DATE__;
  const wchar_t wrapperBuildTime[]        = L"" __TIME__;
  const wchar_t wrapperJavacTargetVersion[] = L"unknown";
#else
  #ifdef _WIN64
    const char wrapperBits[]  = "64";
    const char wrapperArch[]  = "x86_64";
  #else
    const char wrapperBits[]  = "32";
    const char wrapperArch[]  = "x86";
  #endif
  const char wrapperVersionRoot[]      = "dev";
  const char wrapperVersion[]          = "dev";
  const char wrapperOS[]               = "windows";
  const char wrapperReleaseDate[]      = "00000000";
  const char wrapperReleaseTime[]      = "0000";
  const char wrapperBuildDate[]        = __DATE__;
  const char wrapperBuildTime[]        = __TIME__;
  const char wrapperJavacTargetVersion[] = "unknown";
#endif
