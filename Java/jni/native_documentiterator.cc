//
//  native_documentiterator.cc
//  CBForest
//
//  Created by Jens Alfke on 9/12/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#include "com_couchbase_cbforest_DocumentIterator.h"
#include "native_glue.hh"
#include "c4DocEnumerator.h"
#include <errno.h>
#include <vector>

using namespace cbforest::jni;

JNIEXPORT jlong JNICALL Java_com_couchbase_cbforest_DocumentIterator_initEnumerateAllDocs
        (JNIEnv *env, jclass clazz, jlong dbHandle, jstring jStartDocID, jstring jEndDocID,
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
        (JNIEnv *env, jclass clazz, jlong dbHandle, jobjectArray jdocIDs, jint optionFlags)
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
        (JNIEnv *env, jclass clazz, jlong dbHandle, jlong since, jint optionFlags)
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

JNIEXPORT jboolean JNICALL Java_com_couchbase_cbforest_DocumentIterator_next
(JNIEnv *env, jclass clazz, jlong handle)
{
    auto e = (C4DocEnumerator*)handle;
    if (!e)
        return 0;
    C4Error error;
    if (c4enum_next(e, &error)) {
        return true;
    } else if (error.code == 0) {
        c4enum_free(e);  // automatically free at end, to save a JNI call to free()
    } else {
        throwError(env, error);
    }
    return false;
}

JNIEXPORT jlong JNICALL Java_com_couchbase_cbforest_DocumentIterator_getDocumentHandle
(JNIEnv *env, jclass clazz, jlong handle)
{
    auto e = (C4DocEnumerator*)handle;
    if (!e)
        return 0;
    C4Error error;
    auto doc = c4enum_getDocument(e, &error);
    if (!doc) {
        throwError(env, error);
    }
    return (jlong)doc;
}

JNIEXPORT void JNICALL Java_com_couchbase_cbforest_DocumentIterator_getDocumentInfo
(JNIEnv *env, jclass clazz, jlong handle, jobjectArray ids, jlongArray numbers)
{
    auto e = (C4DocEnumerator*)handle;
    C4DocumentInfo info;
    if (!e || !c4enum_getDocumentInfo(e, &info)) {
        memset(&info, 0, sizeof(info));
    }
    env->SetObjectArrayElement(ids, 0, toJString(env, info.docID));
    env->SetObjectArrayElement(ids, 1, toJString(env, info.revID));
    jlong flagsAndSequence[2] = {info.flags, (jlong)info.sequence};
    env->SetLongArrayRegion(numbers, 0, 2, flagsAndSequence);
}

JNIEXPORT void JNICALL Java_com_couchbase_cbforest_DocumentIterator_free
(JNIEnv *env, jclass clazz, jlong handle)
{
    c4enum_free((C4DocEnumerator*)handle);
}
