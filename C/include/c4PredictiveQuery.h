//
// c4PredictiveQuery.h
//
// Copyright © 2018 Couchbase. All rights reserved.
//

#pragma once
#include "c4Base.h"
#include "fleece/Fleece.h"

#ifdef __cplusplus
extern "C" {
#endif

    /** \defgroup PredictiveQuery  Predictive (ML) Query
        @{ */


    typedef struct {
        void* modelInternal;

        C4SliceResult (*predict)(void* modelInternal, FLValue C4NONNULL input, C4Error *error);
    } C4PredictiveModel;

    void c4pred_registerModel(const char *name, C4PredictiveModel);
    bool c4pred_unregisterModel(const char *name);


    /** @} */

#ifdef __cplusplus
}
#endif
