#include "threading_alt.h"
#include "mbedtls/threading.h"

#if defined(_WIN32) && defined(MBEDTLS_THREADING_ALT)

extern "C" {
static void windows_mutex_init(mbedtls_threading_mutex_t* mutex) {
    if ( mutex == NULL ) { return; }

    InitializeCriticalSection(&mutex->MBEDTLS_PRIVATE(mutex));
}

static void windows_mutex_free(mbedtls_threading_mutex_t* mutex) {
    if ( mutex == NULL ) { return; }

    DeleteCriticalSection(&mutex->MBEDTLS_PRIVATE(mutex));
}

// NOTE: The lock and unlock depend on windows_mutex_init being called first.
// That is out of our hands though.  Failure to do so is undefined behavior.
static int windows_mutex_lock(mbedtls_threading_mutex_t* mutex) {
    if ( mutex == NULL ) { return MBEDTLS_ERR_THREADING_BAD_INPUT_DATA; }

    EnterCriticalSection(&mutex->MBEDTLS_PRIVATE(mutex));
    return 0;
}

static int windows_mutex_unlock(mbedtls_threading_mutex_t* mutex) {
    if ( mutex == NULL ) { return MBEDTLS_ERR_THREADING_BAD_INPUT_DATA; }

    LeaveCriticalSection(&mutex->MBEDTLS_PRIVATE(mutex));
    return 0;
}
}

struct MbedTLSInit {
    MbedTLSInit() {
        mbedtls_threading_set_alt(windows_mutex_init, windows_mutex_free, windows_mutex_lock, windows_mutex_unlock);
    }

    ~MbedTLSInit() { mbedtls_threading_free_alt(); }
};

static MbedTLSInit sMbedTLSInit;
#endif
