//
//  c4Observer.h
//  LiteCore
//
//  Created by Jens Alfke on 11/4/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#pragma once

#include "c4Database.h"

#ifdef __cplusplus
extern "C" {
#endif


    /** \defgroup Observer Database and Document Observers
        @{ */

    typedef struct c4DatabaseObserver C4DatabaseObserver;

    typedef void (*C4DatabaseObserverCallback)(C4DatabaseObserver*,
                                               void *context);

    C4DatabaseObserver* c4dbobs_create(C4Database*,
                                       C4SequenceNumber since,
                                       C4DatabaseObserverCallback,
                                       void *context) C4API;

    uint32_t c4dbobs_getChanges(C4DatabaseObserver *obs,
                                C4Slice outDocIDs[],
                                uint32_t maxChanges,
                                C4SequenceNumber* outLastSequence) C4API;

    void c4dbobs_free(C4DatabaseObserver*) C4API;


    typedef struct c4DocumentObserver C4DocumentObserver;

    typedef void (*C4DocumentObserverCallback)(C4DocumentObserver*,
                                               C4Slice docID,
                                               C4SequenceNumber,
                                               void *context);

    C4DocumentObserver* c4docobs_create(C4Database*,
                                        C4Slice docID,
                                        C4DocumentObserverCallback,
                                        void *context) C4API;

    void c4docobs_free(C4DocumentObserver*) C4API;

    /** @} */
#ifdef __cplusplus
}
#endif
