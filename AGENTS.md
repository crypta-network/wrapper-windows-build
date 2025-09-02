# Agents Guide: Windows Native Build Updates

This document summarizes the current uncommitted native code changes, their intent, and clear next‑actions for collaborators and automation agents working on this repository.

## Scope

Windows build reliability and 64‑bit safety for the Java Service Wrapper native components under `src/c/`. Adds portable build files (CMake and NMake) and fixes format‑string issues that could break or misreport on 64‑bit Windows.

## What Changed

- wrapper.c: Windows portability and MSVC quality‑of‑life
  - Define `tzname`, `timezone`, `daylight` to their MSVCRT variants to avoid missing symbol issues.
  - Define `_CRT_SECURE_NO_WARNINGS` to suppress MSVC deprecation noise.
  - Minor include ordering/whitespace normalization.

- wrapper_win.c: 64‑bit safe exception logging
  - Log `ExceptionInformation[i]` (type `ULONG_PTR`) using `%Iu` and cast to `size_t` to be correct on both 32‑ and 64‑bit builds.

- wrapperjni_win.c: 64‑bit safe exception logging (JNI side)
  - Same `%Iu`/`size_t` fix applied to JNI exception logging.

- wrapperinfo.h: const correctness and definition form
  - Declarations changed from `extern TCHAR *...` to `extern const TCHAR ...[];` so these symbols are immutable string arrays rather than writable pointers.
  - Rationale: aligns with a simple, link‑friendly definition strategy and avoids accidental writes.

- src/c/wrapperinfo.c: new fallback implementation (untracked before)
  - Provides sane default values when building outside the Ant token‑replacement flow used by `wrapperinfo.c.in`.
  - Emits `const` array definitions for both ANSI and UNICODE builds; uses `__DATE__`/`__TIME__` for build stamps.

- src/c/CMakeLists.txt: portable Windows build
  - Builds `wrapper.exe` and `wrapper.dll` into `bin/` and `lib/` at repo root.
  - Uses `find_package(JNI)` with fallback to `JAVA_HOME` for headers.
  - Centralizes common sources in `WRAPPER_COMMON_SOURCES` and links required Windows libs.

- src/c/Makefile-windows-x86-64.nmake: legacy NMake build
  - Visual C++ (x64) profile for building the EXE and JNI DLL with resource embedding and output to `bin/` and `lib/`.

## Why It Matters

- Correctness on 64‑bit: Printing pointer‑sized fields with 32‑bit formats is undefined and misleading. The `%Iu`/`size_t` change fixes that.
- Build without Ant: The fallback `wrapperinfo.c` plus CMake/NMake files enable local Windows builds (e.g., VS Developer Prompt) without the Ant token substitution step.
- Safer globals: `const` string arrays remove accidental writes and are friendlier to modern linkers.

## Build Instructions (Windows)

Prerequisites
- MSVC (Visual Studio Build Tools) with C toolchain.
- JDK installed. Either set `JAVA_HOME` or ensure CMake can find JNI.

Using CMake (recommended)
- From a VS Developer Command Prompt that matches your target:
  - x64: use the x64 Native Tools prompt
  - ARM64: use the ARM64 Native Tools prompt
- Commands:
  - `cmake -S src/c -B build -G "NMake Makefiles" -DUNICODE=ON`
  - `cmake --build build --config Release`
- Artifacts: `bin/wrapper.exe` and `lib/wrapper.dll`.

Using NMake (legacy files)
- x64:
  - From a VS x64 Native Tools Command Prompt:
    - `cd src/c`
    - `set JAVA_HOME=C:\\Program Files\\Java\\jdk-<version>` (if not already set)
    - `nmake /f Makefile-windows-x86-64.nmake all`
- ARM64:
  - From a VS ARM64 Native Tools Command Prompt:
    - `cd src/c`
    - `set JAVA_HOME=C:\\Program Files\\Java\\jdk-<version>` (if not already set)
    - `nmake /f Makefile-windows-arm-64.nmake all`
- Artifacts: `..\\..\\bin\\wrapper.exe` and `..\\..\\lib\\wrapper.dll`.

## Compatibility Notes

- Generated vs fallback `wrapperinfo.c`:
  - The header now declares `extern const TCHAR ...[];` (arrays).
  - The template `wrapperinfo.c.in` currently defines writable pointers (`TCHAR *`). If Ant generates `wrapperinfo.c` from the template as‑is, the types will mismatch.
  - Short‑term: keep using the new fallback `src/c/wrapperinfo.c` for local builds.
  - Long‑term: update `src/c/wrapperinfo.c.in` to define `const` arrays (see TODO below) so both paths agree.

- UNICODE/ANSI:
  - CMake enables `UNICODE`/`_UNICODE`; the fallback `wrapperinfo.c` emits wide strings accordingly.

## Open TODOs (Prioritized)

1) Align template with header
- Edit `src/c/wrapperinfo.c.in` so all variables are `const TCHAR ...[]` (arrays) to match `wrapperinfo.h`.

2) Decide build system ownership
- Commit the new `CMakeLists.txt` and NMake file if we want them supported long‑term. Otherwise, gate them behind a `windows-build/` subfolder.

3) Validate 64‑bit logging fixes
- Add a small unit/integration smoke that exercises the SEH/JNI exception logging path on x64 and verifies formatting doesn’t truncate.

4) CI for Windows
- GitHub Actions workflow builds x64 on `windows-2025` and ARM64 on `windows-11-arm`.
- Note: `windows-latest` migrates from Windows Server 2022 to 2025 beginning 2025-09-02; pin to `windows-2025` to avoid image drift.

5) Cleanup defines
- Revisit `_WIN32_WINNT=0x0501` (XP era) in both NMake and CMake; bump to a supported baseline if we no longer target XP.

## Quick Verification Checklist

- Build succeeds via CMake on a clean Windows host with JDK installed.
- `bin/wrapper.exe` and `lib/wrapper.dll` produced; both link to expected Windows libraries.
- No warnings for `%ld` vs pointer‑sized values in `wrapper_win.c`/`wrapperjni_win.c` on x64.
- If `wrapperinfo.c` is regenerated by Ant, types still match `wrapperinfo.h` (post‑TODO fix).

## Suggested Commit Messages

- Native: make exception logging 64‑bit safe (%Iu/size_t) on Windows.
- Native: add Windows CMake + NMake builds; fallback wrapperinfo.c for non‑Ant builds.
- Native: make wrapperinfo strings `const` arrays; align header and template.

## Notes for Agents

- Prefer CMake for automation; it detects JNI and places outputs in stable locations.
- When editing headers with exported symbols, grep for the symbol across the tree to catch template‐generated sources (see `wrapperinfo.c.in`).
- For format‑string changes touching Windows types, validate with `/W4` and 64‑bit builds; `%Iu` is the MSVC portable specifier for `size_t`.
