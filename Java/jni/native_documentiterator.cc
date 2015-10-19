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
#include <errno.h>
#include <vector>

using namespace forestdb::jni;

JNIEXPORT jlong JNICALL Java_com_couchbase_cbforest_DocumentIterator_initEnumerateAllDocs
        (JNIEnv *env, jobject self, jlong dbHandle, jstring jStartDocID, jstring jEndDocID,
         jint skip, jint optionFlags)
{
    jstringSlice startDocID(env, jStartDocID);
    jstringSlice endDocID(env, jEndDocID);
    const C4EnumeratorOptions options = {unsigned(skip), C4EnumeratorFlags(optionFlags)};
    C4Error error;
    C4DocEnumerator *e = c4db_enumerateAllDocs((C4Database*)dbHandle, startDocID, endDocID, &options, &error);
    if (!e) {
        throwError(env, error);
        return 0;
    }
    return (jlong)e;
}

JNIEXPORT jlong JNICALL Java_com_couchbase_cbforest_DocumentIterator_initEnumerateSomeDocs
        (JNIEnv *env, jobject self, jlong dbHandle, jobjectArray jdocIDs, jint optionFlags)
{
    // Convert jdocIDs, a Java String[], to a C array of C4Slice:

    jsize n = env->GetArrayLength(jdocIDs);

    C4Slice* docIDs = (C4Slice*)::malloc(sizeof(C4Slice) * n);
    if(docIDs  == NULL){
        throwError(env, C4Error{POSIXDomain, errno});
        return 0;
    }

    std::vector<jstringSlice *> keeper;
    for (jsize i = 0; i < n; i++) {
        jstring js = (jstring)env->GetObjectArrayElement(jdocIDs, i);
        jstringSlice* item = new jstringSlice(env, js);
        docIDs[i] = *item;
        keeper.push_back(item); // so its memory won't be freed
    }

    const C4EnumeratorOptions options = {unsigned(0), C4EnumeratorFlags(optionFlags)};
    C4Error error;
    C4DocEnumerator *e = c4db_enumerateSomeDocs((C4Database*)dbHandle, docIDs, n, &options,
                                                &error);

    // release memory
    for(jsize i = 0; i < n; i++){
        delete keeper.at(i);
    }
    keeper.clear();
    ::free(docIDs);

    if (!e) {
        throwError(env, error);
        return 0;
    }
    return (jlong)e;
}


JNIEXPORT jlong JNICALL Java_com_couchbase_cbforest_DocumentIterator_initEnumerateChanges
        (JNIEnv *env, jobject self, jlong dbHandle, jlong since, jint optionFlags)
{
    const C4EnumeratorOptions options = {unsigned(0), C4EnumeratorFlags(optionFlags)};
    C4Error error;
    C4DocEnumerator *e = c4db_enumerateChanges((C4Database*)dbHandle, since, &options, &error);
    if (!e) {
        throwError(env, error);
        return 0;
    }
    return (jlong)e;
}

JNIEXPORT jlong JNICALL Java_com_couchbase_cbforest_DocumentIterator_nextDocumentHandle
(JNIEnv *env, jclass clazz, jlong handle)
{
    auto e = (C4DocEnumerator*)handle;
    if (!e)
        return 0;
    C4Error error;
    auto doc = c4enum_nextDocument(e, &error);
    if (!doc) {
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
