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
        @{
        This API allows you to register a machine-learning model with LiteCore. It can then be
        invoked from a query by the PREDICTION() function. The model results can be indexed, to
        speed up queries, using the index type `kC4PredictiveIndex`.

        A model is implemented with a callback that will be invoked during a query.
        The callback takes as input a set of named parameters, which are passed as a Fleece
        dictionary. It produces a set of named results, which it returns as another Fleece
        dictionary, encoded as data. This matches the APIs of libraries like CoreML and TensorFlow.

        ML models often expect or produce multi-dimensional numeric arrays, which obviously aren't
        directly supported by Fleece nor JSON. It's up to you to translate them appropriately.
        The most direct translation is of arrays of arrays (of arrays...) of numbers, but this
        representation is pretty verbose and expensive to translate. You may want to store the
        raw array data in a blob instead, but this has its own issues like endianness and the
        need to know the array dimensions up-front.

        The most common use of a multi-dimensional array is as an image pixmap; in this case the
        natural Fleece input is a blob containing encoded image data in a common format like JPEG
        or PNG. Again, you're responsible for decoding the image data and rendering it into the
        appropriate binary array. (Your ML library may assist you here; for example, CoreML works
        with the Vision framework, so all you have to do is pass in the encoded image data and
        the frameworks do the rest.)

        You must be vigilant about invalid data, since the prediction query may well be run on
        documents that don't have the expected schema. Obviously the callback should not crash nor
        corrupt memory. It should also probably not return an error if input parameters are
        missing or of the wrong type; instead it should return null without storing an error
        value in the `error` parameter. The reason is that, if it returns an error, this will
        propagate all the way up the query and cause the entire query to fail. Usually it's more
        appropriate to return a null slice, which equates to a result of MISSING, which will just
        cause this document to fail the query condition. */


    /** Configuration struct for registering a predictive model. */
    typedef struct {
        /** A pointer to any external data needed by the `prediction` callback, which will receive
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
        C4SliceResult (*prediction)(void* context, FLDict C4NONNULL input, C4Error *error);
    } C4PredictiveModel;


    /** Registers a predictive model, under a name. The model can now be invoked within a query
        by calling `prediction(_name_, _input_)`. The model remains registered until it's explicitly
        unregistered, or another model is registered with the same name. */
    void c4pred_registerModel(const char *name, C4PredictiveModel);

    /** Unregisters whatever model was last registered with this name. */
    bool c4pred_unregisterModel(const char *name);


    /** @} */

#ifdef __cplusplus
}
#endif
