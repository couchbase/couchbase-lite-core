#pragma once

#ifndef __cplusplus
#include <stddef.h>
#else
#include <cstddef>
extern "C"
{
#endif
size_t strlcat(char *dst, const char *src, size_t dsize);

#ifdef __cplusplus
}
#endif
