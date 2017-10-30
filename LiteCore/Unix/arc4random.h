#pragma once
#include <stdint.h>

uint32_t arc4random(void);
void arc4random_buf(void *buffer, size_t size);
