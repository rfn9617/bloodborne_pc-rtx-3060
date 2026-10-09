// bbport: AMD's FSR 3.1 SDK sources (ffx-1.1.4) use MSVC's _countof. Recent MSYS2 CLANG64
// toolchains no longer provide it to these strict C++17 sources, which broke the Windows
// build; forced into that library only, and only when the headers do not define it.
#pragma once
#include <stdlib.h>
#ifndef _countof
#define _countof(array) (sizeof(array) / sizeof((array)[0]))
#endif
