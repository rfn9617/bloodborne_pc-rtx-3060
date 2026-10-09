// bbport (Windows): newer MSYS2 CLANG64 toolchains ship a libc++ that no longer includes the
// common standard headers through other ones. Sources written against the older behaviour
// (sirit: std::abort without <cstdlib>; the FSR SDK: log2 and placement new without <cmath>
// and <new>) stop compiling. Forced into every C++ file of the Windows build (gpu/
// CMakeLists.txt); only standard headers, so nothing else changes.
#pragma once
#ifdef __cplusplus
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>
#endif
