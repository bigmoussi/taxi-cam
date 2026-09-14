// Both targets use Clang's Windows x64 varargs implementation.
#pragma once
typedef __builtin_va_list va_list;
#define va_start(v, p) __builtin_va_start(v, p)
#define va_end(v) __builtin_va_end(v)
#define va_arg(v, t) __builtin_va_arg(v, t)
#define va_copy(d, s) __builtin_va_copy(d, s)
