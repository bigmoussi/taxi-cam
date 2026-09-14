// Compile-only Windows x64 type definitions. Never used to build the add-on.
#pragma once
using size_t = unsigned long long;
using ptrdiff_t = long long;
#define NULL 0
#define offsetof(T, F) __builtin_offsetof(T, F)
