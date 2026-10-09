// bbport: AMD's FSR 3.1 SDK sources (ffx-1.1.4) use MSVC CRT extensions (_countof,
// swprintf_s). Recent MSYS2 CLANG64 headers no longer declare them for these strict C++17
// sources, which broke the Windows CI build (the toolchain installed on 2026-10-08 still
// does). Forced into that library only. The standard headers come first, so their include
// guards keep later includes from redeclaring the names redirected below.
#pragma once
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#ifdef __cplusplus
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#endif

#ifndef _countof
#define _countof(array) (sizeof(array) / sizeof((array)[0]))
#endif

static inline int bb_ffx_swprintf_s(wchar_t* buffer, size_t count, const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    const int written = vswprintf(buffer, count, format, args);
    va_end(args);
    if (written < 0 && buffer && count) {
        buffer[0] = L'\0';
    }
    return written;
}
#define swprintf_s bb_ffx_swprintf_s
