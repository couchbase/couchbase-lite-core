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
extern "C" {
#endif

void c4log_warnOnErrors(bool) C4API;

C4Document* c4doc_getForPut(C4Database *database,
                            C4Slice docID,
                            C4Slice parentRevID,
                            bool deleting,
                            bool allowConflict,
                            C4Error *outError) C4API;
    
#ifdef __cplusplus
}
#endif
