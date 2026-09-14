// Compile-only declarations. No functions from this header are linked or run.
#pragma once
#include "stddef.h"
extern "C" {
void* memset(void*, int, size_t);
void* memmove(void*, const void*, size_t);
void* memcpy(void*, const void*, size_t);
int memcmp(const void*, const void*, size_t);
size_t strlen(const char*);
char* strchr(const char*, int);
char* strcpy(char*, const char*);
int strcmp(const char*, const char*);
}
