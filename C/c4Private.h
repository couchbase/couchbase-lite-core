//
//  c4Private.h
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 1/21/16.
//  Copyright (c) 2016 Couchbase. All rights reserved.
//

#pragma once
#include "c4Document.h"

#ifdef __cplusplus
#include <atomic>
#else
#include <stdatomic.h>
#endif

#ifdef __cplusplus
extern "C" {

extern std::atomic_int gC4InstanceCount;

#else
extern atomic_int gC4InstanceCount;
#endif

C4Error c4error_make(C4ErrorDomain domain, int code, C4String message) C4API;

void c4log_warnOnErrors(bool) C4API;

C4Document* c4doc_getForPut(C4Database *database,
                            C4Slice docID,
                            C4Slice parentRevID,
                            bool deleting,
                            bool allowConflict,
                            C4Error *outError) C4API;
    
#ifdef __cplusplus
}




/** Base class that keeps track of the total instance count of it and all subclasses.
    This is useful for leak detection. */
class C4InstanceCounted {
public:
    C4InstanceCounted()   {++gC4InstanceCount;}
    ~C4InstanceCounted()  {--gC4InstanceCount;}
};

#endif
