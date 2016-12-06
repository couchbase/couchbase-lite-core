#pragma once

#ifdef __cplusplus
extern "C" {
#endif

    void arc4random_buf(void *buffer, int size);

    uint32_t arc4random();

#ifdef __cplusplus
}
#endif