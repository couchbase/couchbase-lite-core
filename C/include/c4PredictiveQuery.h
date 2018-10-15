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

    /** \defgroup PredictiveQuery  Predictive (Machine-Learning) Query
        @{ */


    /** Configuration struct for registering a predictive model. */
    typedef struct {
        /** A pointer to any external data needed by the `predict` function, which will receive
            this as its first parameter. */
        void* context;

        /** Called from within a query (or document indexing) to run the prediction.
            @param context  The value of the C4PredictiveModel's `context` field;
                    typically a pointer to external data needed by the implementation.
            @param input  The input dictionary from the query.
            @param error  Store an error here on failure. It is NOT a failure for input parameters
                    to be missing or the wrong type, since this can easily happen when the
                    query reaches a document that doesn't contain input data, or if the document's
                    schema is incorrect. This should not abort the entire query! Instead just
                    return a null slice.
            @return  The output of the prediction function, encoded as a Fleece dictionary,
                    or as {NULL, 0} if there is no output. */
        C4SliceResult (*predict)(void* context, FLDict C4NONNULL input, C4Error *error);
    } C4PredictiveModel;


    /** Registers a predictive model, under a name. The model can now be invoked within a query
        by calling `predict(_name_, _input_)`. The model remains registered until it's explicitly
        unregistered, or another model is registered with the same name. */
    void c4pred_registerModel(const char *name, C4PredictiveModel);

    /** Unregisters whatever model was last registered with this name. */
    bool c4pred_unregisterModel(const char *name);


    /** @} */

#ifdef __cplusplus
}
#endif
