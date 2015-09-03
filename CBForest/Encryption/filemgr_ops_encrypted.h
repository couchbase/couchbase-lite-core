//
//  filemgr_ops_encrypted.h
//  CBForest
//
//  Created by Jens Alfke on 7/27/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//

#ifndef __CBForest__filemgr_ops_encrypted__
#define __CBForest__filemgr_ops_encrypted__

#include "fdb_errors.h"

#ifdef __cplusplus
extern "C" {
#endif

    enum {
        FDB_RESULT_ENCRYPTION_ERROR  = -100,
        FDB_RESULT_INVALID_IO_PARAMS = -101
    };

    typedef struct {
        uint8_t bytes[32];
    } EncryptionKey;

    void fdb_registerEncryptionKey(const char *pathname, const EncryptionKey *key);

    EncryptionKey fdb_randomEncryptionKey(void);

    fdb_status fdb_copy_open_file(const char *fromPath, const char *toPath, const EncryptionKey* toKey);

#ifdef __cplusplus
}
#endif

#endif /* __CBForest__filemgr_ops_encrypted__ */
