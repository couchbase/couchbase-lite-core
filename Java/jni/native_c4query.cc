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
#include <c4.h>
#include <c4Base.h>
#include "com_couchbase_litecore_C4Query.h"
#include "com_couchbase_litecore_C4QueryEnumerator.h"
#include "native_glue.hh"


using namespace litecore;
using namespace litecore::jni;

// ----------------------------------------------------------------------------
// com_couchbase_litecore_C4Query
// ----------------------------------------------------------------------------

/*
 * Class:     com_couchbase_litecore_C4Query
 * Method:    init
 * Signature: (JLjava/lang/String;)J
 */
JNIEXPORT jlong JNICALL
Java_com_couchbase_litecore_C4Query_init(JNIEnv *env, jclass clazz, jlong db, jstring jexpr) {
    jstringSlice expr(env, jexpr);
    C4Error error = {};
    C4Query *query = c4query_new((C4Database *) db, expr, &error);
    if (!query)
        throwError(env, error);
    return (jlong) query;
}

/*
 * Class:     com_couchbase_litecore_C4Query
 * Method:    free
 * Signature: (J)V
 */
JNIEXPORT void JNICALL
Java_com_couchbase_litecore_C4Query_free(JNIEnv *env, jclass clazz, jlong jquery) {
    c4query_free((C4Query *) jquery);
}

/*
 * Class:     com_couchbase_litecore_C4Query
 * Method:    explain
 * Signature: (J)Ljava/lang/String;
 */
JNIEXPORT jstring JNICALL
Java_com_couchbase_litecore_C4Query_explain(JNIEnv *env, jclass clazz, jlong jquery) {
    C4StringResult result = c4query_explain((C4Query *) jquery);
    jstring jstr = toJString(env, result);
    c4slice_free(result);
    return jstr;
}


/*
 * Class:     com_couchbase_litecore_C4Query
 * Method:    columnCount
 * Signature: (J)I
 */
JNIEXPORT jint JNICALL
Java_com_couchbase_litecore_C4Query_columnCount(JNIEnv *env, jclass clazz, jlong jquery) {
    return c4query_columnCount((C4Query *) jquery);
}

/*
 * Class:     com_couchbase_litecore_C4Query
 * Method:    nameOfColumn
 * Signature: (JI)Ljava/lang/String;
 */
JNIEXPORT jstring JNICALL
Java_com_couchbase_litecore_C4Query_nameOfColumn(JNIEnv *env, jclass clazz, jlong jquery,
                                                 jint jcol) {
    C4StringResult result = c4query_nameOfColumn((C4Query *) jquery, jcol);
    jstring jstr = toJString(env, result);
    c4slice_free(result);
    return jstr;
}

/*
 * Class:     com_couchbase_litecore_C4Query
 * Method:    run
 * Signature: (JJJZLjava/lang/String;)J
 */
JNIEXPORT jlong JNICALL
Java_com_couchbase_litecore_C4Query_run(JNIEnv *env, jclass clazz,
                                        jlong jquery,
                                        jboolean jrankFullText,
                                        jstring jencodedParameters) {
    C4QueryOptions options = {
            (bool) jrankFullText
    };
    jstringSlice encodedParameters(env, jencodedParameters);
    C4Error error = {};
    C4QueryEnumerator *e = c4query_run((C4Query *) jquery, &options, encodedParameters, &error);
    if (!e)
        throwError(env, error);
    return (jlong) e;
}

/*
 * Class:     com_couchbase_litecore_C4Query
 * Method:    getFullTextMatched
 * Signature: (JLjava/lang/String;J)[B
 */
JNIEXPORT jbyteArray JNICALL
Java_com_couchbase_litecore_C4Query_getFullTextMatched(JNIEnv *env, jclass clazz, jlong jquery,
                                                       jstring jdocid, jlong jseq) {
    jstringSlice docID(env, jdocid);
    C4SliceResult s = c4query_fullTextMatched((C4Query *) jquery, docID,
                                              (C4SequenceNumber) jseq, nullptr);
    jbyteArray res = toJByteArray(env, s);
    c4slice_free(s);
    return res;
}

// ----------------------------------------------------------------------------
// com_couchbase_litecore_C4QueryEnumerator
// ----------------------------------------------------------------------------

/*
 * Class:     com_couchbase_litecore_C4QueryEnumerator
 * Method:    getFullTextMatched
 * Signature: (J)[B
 */
JNIEXPORT jbyteArray JNICALL
Java_com_couchbase_litecore_C4QueryEnumerator_getFullTextMatched(JNIEnv *env, jclass clazz,
                                                                 jlong handle) {
    C4SliceResult s = c4queryenum_fullTextMatched((C4QueryEnumerator *) handle, nullptr);
    jbyteArray res = toJByteArray(env, s);
    c4slice_free(s);
    return res;
}

/*
 * Class:     com_couchbase_litecore_C4QueryEnumerator
 * Method:    next
 * Signature: (J)Z
 */
JNIEXPORT jboolean JNICALL
Java_com_couchbase_litecore_C4QueryEnumerator_next(JNIEnv *env, jclass clazz, jlong handle) {
    if (!handle)
        return false;
    C4Error error = {};
    jboolean result = c4queryenum_next((C4QueryEnumerator *) handle, &error);
    if (!result) {
        // NOTE: Please keep folowing line of code for a while.
        // At end of iteration, proactively free the enumerator:
        // c4queryenum_free((C4QueryEnumerator *) handle);
        if (error.code != 0)
            throwError(env, error);
    }
    return result;
}

/*
 * Class:     com_couchbase_litecore_C4QueryEnumerator
 * Method:    getRowCount
 * Signature: (J)J
 */
JNIEXPORT jlong JNICALL
Java_com_couchbase_litecore_C4QueryEnumerator_getRowCount(JNIEnv *env, jclass clazz, jlong handle) {
    C4Error error = {};
    int64_t res = c4queryenum_getRowCount((C4QueryEnumerator *) handle, &error);
    if (res == -1)
        throwError(env, error);
    return (jlong) res;
}

/*
 * Class:     com_couchbase_litecore_C4QueryEnumerator
 * Method:    seek
 * Signature: (JJ)Z
 */
JNIEXPORT jboolean JNICALL
Java_com_couchbase_litecore_C4QueryEnumerator_seek(JNIEnv *env, jclass clazz, jlong handle,
                                                   jlong rowIndex) {
    if (!handle)
        return false;
    C4Error error = {};
    jboolean result = c4queryenum_seek((C4QueryEnumerator *) handle, (uint64_t) rowIndex, &error);
    if (!result)
        throwError(env, error);
    return result;
}

/*
 * Class:     com_couchbase_litecore_C4QueryEnumerator
 * Method:    refresh
 * Signature: (J)J
 */
JNIEXPORT jlong JNICALL
Java_com_couchbase_litecore_C4QueryEnumerator_refresh(JNIEnv *env, jclass clazz, jlong handle) {
    C4Error error = {.code=0};
    C4QueryEnumerator *result = c4queryenum_refresh((C4QueryEnumerator *) handle, &error);
    // NOTE: if result is null, it indicates no update. it is not error.
    if (error.code != 0)
        throwError(env, error);
    return (jlong) result;
}

/*
 * Class:     com_couchbase_litecore_C4QueryEnumerator
 * Method:    close
 * Signature: (J)V
 */
JNIEXPORT void JNICALL
Java_com_couchbase_litecore_C4QueryEnumerator_close(JNIEnv *env, jclass clazz, jlong handle) {
    if (!handle)
        return;
    c4queryenum_close((C4QueryEnumerator *) handle);
}

/*
 * Class:     com_couchbase_litecore_C4QueryEnumerator
 * Method:    free
 * Signature: (J)V
 */
JNIEXPORT void JNICALL
Java_com_couchbase_litecore_C4QueryEnumerator_free(JNIEnv *env, jclass clazz, jlong handle) {
    if (!handle)
        return;
    c4queryenum_free((C4QueryEnumerator *) handle);
}

/*
 * Class:     com_couchbase_litecore_C4QueryEnumerator
 * Method:    getDocID
 * Signature: (J)Ljava/lang/String;
 */
JNIEXPORT jstring JNICALL
Java_com_couchbase_litecore_C4QueryEnumerator_getDocID(JNIEnv *env, jclass clazz, jlong handle) {
    if (!handle)
        return nullptr;
    return toJString(env, ((C4QueryEnumerator *) handle)->docID);
}

/*
 * Class:     com_couchbase_litecore_C4QueryEnumerator
 * Method:    getDocSequence
 * Signature: (J)J
 */
JNIEXPORT jlong JNICALL
Java_com_couchbase_litecore_C4QueryEnumerator_getDocSequence(JNIEnv *env, jclass clazz,
                                                             jlong handle) {
    if (!handle)
        return 0;
    return ((C4QueryEnumerator *) handle)->docSequence;
}

/*
 * Class:     com_couchbase_litecore_C4QueryEnumerator
 * Method:    getRevID
 * Signature: (J)Ljava/lang/String;
 */
JNIEXPORT jstring JNICALL
Java_com_couchbase_litecore_C4QueryEnumerator_getRevID(JNIEnv *env, jclass clazz, jlong handle) {
    if (!handle)
        return nullptr;
    return toJString(env, ((C4QueryEnumerator *) handle)->revID);
}

/*
 * Class:     com_couchbase_litecore_C4QueryEnumerator
 * Method:    getDocFlags
 * Signature: (J)J
 */
JNIEXPORT jlong JNICALL
Java_com_couchbase_litecore_C4QueryEnumerator_getDocFlags(JNIEnv *env, jclass clazz, jlong handle) {
    if (!handle)
        return 0;
    return ((C4QueryEnumerator *) handle)->docFlags;
}

/*
 * Class:     com_couchbase_litecore_C4QueryEnumerator
 * Method:    getColumns
 * Signature: (J)J
 */
JNIEXPORT jlong JNICALL
Java_com_couchbase_litecore_C4QueryEnumerator_getColumns(JNIEnv *env, jclass clazz, jlong handle) {
    if (!handle)
        return 0;
    return (jlong) &((C4QueryEnumerator *) handle)->columns;
}

/*
 * Class:     com_couchbase_litecore_C4QueryEnumerator
 * Method:    getFullTextTermCount
 * Signature: (J)J
 */
JNIEXPORT jlong JNICALL
Java_com_couchbase_litecore_C4QueryEnumerator_getFullTextTermCount(JNIEnv *env, jclass clazz,
                                                                   jlong handle) {
    if (!handle)
        return 0;
    return (jlong) ((C4QueryEnumerator *) handle)->fullTextTermCount;
}

/*
 * Class:     com_couchbase_litecore_C4QueryEnumerator
 * Method:    getFullTextTermIndex
 * Signature: (JJ)J
 */
JNIEXPORT jlong JNICALL
Java_com_couchbase_litecore_C4QueryEnumerator_getFullTextTermIndex(JNIEnv *env, jclass clazz,
                                                                   jlong handle, jlong jpos) {
    if (!handle || jpos >= ((C4QueryEnumerator *) handle)->fullTextTermCount)
        return -1;
    return (jlong) ((C4QueryEnumerator *) handle)->fullTextTerms[jpos].termIndex;
}

/*
 * Class:     com_couchbase_litecore_C4QueryEnumerator
 * Method:    getFullTextTermStart
 * Signature: (JJ)J
 */
JNIEXPORT jlong JNICALL
Java_com_couchbase_litecore_C4QueryEnumerator_getFullTextTermStart(JNIEnv *env, jclass clazz,
                                                                   jlong handle, jlong jpos) {
    if (!handle || jpos >= ((C4QueryEnumerator *) handle)->fullTextTermCount)
        return -1;
    return (jlong) ((C4QueryEnumerator *) handle)->fullTextTerms[jpos].start;
}

/*
 * Class:     com_couchbase_litecore_C4QueryEnumerator
 * Method:    getFullTextTermLength
 * Signature: (JJ)J
 */
JNIEXPORT jlong JNICALL
Java_com_couchbase_litecore_C4QueryEnumerator_getFullTextTermLength(JNIEnv *env, jclass clazz,
                                                                    jlong handle, jlong jpos) {
    if (!handle || jpos >= ((C4QueryEnumerator *) handle)->fullTextTermCount)
        return -1;
    return (jlong) ((C4QueryEnumerator *) handle)->fullTextTerms[jpos].length;
}

/*
 * Class:     com_couchbase_litecore_C4Query
 * Method:    createIndex
 * Signature: (JLjava/lang/String;Ljava/lang/String;ILjava/lang/String;Z)Z
 */
JNIEXPORT jboolean JNICALL
Java_com_couchbase_litecore_C4Query_createIndex(JNIEnv *env, jclass clazz, jlong jdb, jstring jname,
                                                jstring jexpressionsJSON, jint indexType,
                                                jstring jlanguage, jboolean ignoreDiacritics) {
    jstringSlice name(env, jname);
    jstringSlice expressionsJSON(env, jexpressionsJSON);
    jstringSlice language(env, jlanguage);
    C4Error error = {};
    bool res = c4db_createIndex((C4Database *) jdb, name, (C4Slice) expressionsJSON,
                                (C4IndexType) indexType, nullptr, &error);
    if (!res)
        throwError(env, error);
    return res;
}

/*
 * Class:     com_couchbase_litecore_C4Query
 * Method:    deleteIndex
 * Signature: (JLjava/lang/String;I)Z
 */
JNIEXPORT jboolean JNICALL
Java_com_couchbase_litecore_C4Query_deleteIndex(JNIEnv *env, jclass clazz, jlong jdb,
                                                jstring jexpressionsJSON, jint indexType) {
    jstringSlice expressionsJSON(env, jexpressionsJSON);
    C4Error error = {};
    bool res = c4db_deleteIndex((C4Database *) jdb, (C4Slice) expressionsJSON,
                                (C4IndexType) indexType, &error);
    if (!res)
        throwError(env, error);
    return res;
}
