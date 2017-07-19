/**
 * Copyright (c) 2017 Couchbase, Inc. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
 * except in compliance with the License. You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed under the
 * License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
 * either express or implied. See the License for the specific language governing permissions
 * and limitations under the License.
 */
#include <errno.h>
#include "com_couchbase_litecore_C4DocEnumerator.h"
#include "c4DocEnumerator.h"
#include "native_glue.hh"

using namespace litecore;
using namespace litecore::jni;

// ----------------------------------------------------------------------------
// com_couchbase_litecore_C4DocEnumerator
// ----------------------------------------------------------------------------
/*
 * Class:     com_couchbase_litecore_C4DocEnumerator
 * Method:    close
 * Signature: (J)V
 */
JNIEXPORT void JNICALL
Java_com_couchbase_litecore_C4DocEnumerator_close(JNIEnv *env, jclass clazz, jlong handle) {
    c4enum_close((C4DocEnumerator *) handle);
}

/*
 * Class:     com_couchbase_litecore_C4DocEnumerator
 * Method:    free
 * Signature: (J)V
 */
JNIEXPORT void JNICALL
Java_com_couchbase_litecore_C4DocEnumerator_free(JNIEnv *env, jclass clazz, jlong handle) {
    c4enum_free((C4DocEnumerator *) handle);
}

/*
 * Class:     com_couchbase_litecore_C4DocEnumerator
 * Method:    enumerateChanges
 * Signature: (JJJI)J
 */
JNIEXPORT jlong JNICALL
Java_com_couchbase_litecore_C4DocEnumerator_enumerateChanges(JNIEnv *env, jclass clazz, jlong jdb,
                                                             jlong since, jlong jskip,
                                                             jint jflags) {
    const C4EnumeratorOptions options = {uint64_t(jskip), C4EnumeratorFlags(jflags)};
    C4Error error;
    C4DocEnumerator *e = c4db_enumerateChanges((C4Database *) jdb, since, &options, &error);
    if (!e)
        throwError(env, error);
    return (jlong) e;
}

/*
 * Class:     com_couchbase_litecore_C4DocEnumerator
 * Method:    enumerateAllDocs
 * Signature: (JLjava/lang/String;Ljava/lang/String;JI)J
 */
JNIEXPORT jlong JNICALL
Java_com_couchbase_litecore_C4DocEnumerator_enumerateAllDocs(JNIEnv *env, jclass clazz, jlong jdb,
                                                             jstring jStartDocID, jstring jEndDocID,
                                                             jlong jskip, jint jflags) {
    jstringSlice startDocID(env, jStartDocID);
    jstringSlice endDocID(env, jEndDocID);
    const C4EnumeratorOptions options = {uint64_t(jskip), C4EnumeratorFlags(jflags)};
    C4Error error;
    C4DocEnumerator *e = c4db_enumerateAllDocs((C4Database *) jdb, startDocID, endDocID, &options,
                                               &error);
    if (!e)
        throwError(env, error);
    return (jlong) e;
}

/*
 * Class:     com_couchbase_litecore_C4DocEnumerator
 * Method:    enumerateSomeDocs
 * Signature: (J[Ljava/lang/String;JI)J
 */
JNIEXPORT jlong JNICALL
Java_com_couchbase_litecore_C4DocEnumerator_enumerateSomeDocs(JNIEnv *env, jclass clazz, jlong jdb,
                                                              jobjectArray jdocIDs, jlong jskip,
                                                              jint jflags) {
    // Convert jdocIDs, a Java String[], to a C array of C4Slice:

    jsize n = env->GetArrayLength(jdocIDs);

    C4Slice *docIDs = (C4Slice *) ::malloc(sizeof(C4Slice) * n);
    if (docIDs == nullptr) {
        throwError(env, C4Error{POSIXDomain, errno});
        return 0;
    }

    std::vector<jstringSlice *> keeper;
    for (jsize i = 0; i < n; i++) {
        jstring js = (jstring) env->GetObjectArrayElement(jdocIDs, i);
        jstringSlice *item = new jstringSlice(env, js);
        docIDs[i] = *item;
        keeper.push_back(item); // so its memory won't be freed
    }

    const C4EnumeratorOptions options = {uint64_t(jskip), C4EnumeratorFlags(jflags)};
    C4Error error;
    C4DocEnumerator *e = c4db_enumerateSomeDocs((C4Database *) jdb, docIDs, n, &options, &error);

    // release memory
    for (jsize i = 0; i < n; i++) {
        delete keeper.at(i);
    }
    keeper.clear();
    ::free(docIDs);

    if (!e) {
        throwError(env, error);
        return 0;
    }
    return (jlong) e;
}

/*
 * Class:     com_couchbase_litecore_C4DocEnumerator
 * Method:    next
 * Signature: (J)Z
 */
JNIEXPORT jboolean JNICALL
Java_com_couchbase_litecore_C4DocEnumerator_next(JNIEnv *env, jclass clazz, jlong handle) {
    C4Error error = {};
    bool res = c4enum_next((C4DocEnumerator *) handle, &error);
    if (!res && error.code != 0)
        throwError(env, error);
    return (jlong) res;
}

/*
 * Class:     com_couchbase_litecore_C4DocEnumerator
 * Method:    getDocument
 * Signature: (J)J
 */
JNIEXPORT jlong JNICALL
Java_com_couchbase_litecore_C4DocEnumerator_getDocument(JNIEnv *env, jclass clazz, jlong handle) {
    C4Error error = {};
    C4Document *doc = c4enum_getDocument((C4DocEnumerator *) handle, &error);
    if (!doc)
        throwError(env, error);
    return (jlong) doc;
}

/*
 * Class:     com_couchbase_litecore_C4DocEnumerator
 * Method:    nextDocument
 * Signature: (J)J
 */
JNIEXPORT jlong JNICALL
Java_com_couchbase_litecore_C4DocEnumerator_nextDocument(JNIEnv *env, jclass clazz, jlong handle) {
    C4Error error = {};
    C4Document *doc = c4enum_nextDocument((C4DocEnumerator *) handle, &error);
    if (!doc && error.code != 0)
        throwError(env, error);
    return (jlong) doc;
}
