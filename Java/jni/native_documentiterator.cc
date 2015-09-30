//
//  native_documentiterator.cc
//  CBForest
//
//  Created by Jens Alfke on 9/12/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//

#include "com_couchbase_cbforest_DocumentIterator.h"
#include "native_glue.hh"
#include "c4Database.h"

using namespace forestdb::jni;


JNIEXPORT jlong JNICALL Java_com_couchbase_cbforest_DocumentIterator_nextDocumentHandle
(JNIEnv *env, jclass clazz, jlong handle)
{
    auto e = (C4DocEnumerator*)handle;
    if (!e)
        return 0;
    C4Error error;
    auto doc = c4enum_nextDocument(e, &error);
    if (doc) {
        if (error.code == 0)
            c4enum_free(e);  // automatically free at end, to save a JNI call to free()
        else
            throwError(env, error);
    }
    return (jlong)doc;
}


JNIEXPORT void JNICALL Java_com_couchbase_cbforest_DocumentIterator_free
(JNIEnv *env, jclass clazz, jlong handle)
{
    c4enum_free((C4DocEnumerator*)handle);
}
