//
//  c4Private.h
//  CBForest
//
//  Created by Jens Alfke on 1/21/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#ifndef c4Private_h
#define c4Private_h
#include "c4Document.h"

#ifdef __cplusplus
extern "C" {
#endif

C4Document* c4doc_getForPut(C4Database *database,
                            C4Slice docID,
                            C4Slice parentRevID,
                            bool deleting,
                            bool allowConflict,
                            C4Error *outError);
    
#ifdef __cplusplus
}
#endif

#endif /* c4Private_h */
