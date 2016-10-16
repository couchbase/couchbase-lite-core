#pragma once

#ifndef __cplusplus
#include <stdarg.h>
#else
#include <cstdarg>
extern "C"
{
#endif
size_t strlcat(char *dst, const char *src, size_t dsize);

#ifdef __cplusplus
}
#endif