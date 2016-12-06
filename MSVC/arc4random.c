#include <windows.h>
#include <stdint.h>

static HCRYPTPROV _rngProv = 0;

void arc4random_buf(void *buffer, int size)
{
    if (_rngProv == 0) {
        CryptAcquireContext(&_rngProv, NULL, NULL, PROV_RSA_FULL, 0);
    }

    CryptGenRandom(_rngProv, size, (BYTE *)buffer);
}

uint32_t arc4random()
{
    if (_rngProv == 0) {
        CryptAcquireContext(&_rngProv, NULL, NULL, PROV_RSA_FULL, 0);
    }

    uint32_t retVal;
    CryptGenRandom(_rngProv, 4, (BYTE *)&retVal);
    return retVal;
}