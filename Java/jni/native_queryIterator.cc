//
//  native_queryIterator.cc
//  CBForest
//
//  Created by Jens Alfke on 9/17/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#include "com_couchbase_cbforest_QueryIterator.h"
#include "com_couchbase_cbforest_FullTextResult.h"
#include "native_glue.hh"
#include "Collatable.hh"
#include "c4View.h"


using namespace cbforest;
using namespace cbforest::jni;


static jfieldID kHandleField;


bool cbforest::jni::initQueryIterator(JNIEnv *env) {
    jclass queryIterClass = env->FindClass("com/couchbase/cbforest/QueryIterator");
    if (!queryIterClass)
        return false;
    kHandleField = env->GetFieldID(queryIterClass, "_handle", "J");
    return (kHandleField != NULL);
}


JNIEXPORT jboolean JNICALL Java_com_couchbase_cbforest_QueryIterator_next
  (JNIEnv *env, jclass clazz, jlong handle)
{
    auto e = (C4QueryEnumerator*)handle;
    if (!e)
        return false;
    C4Error error;
    jboolean result = c4queryenum_next(e, &error);
    if (!result) {
        // At end of iteration, proactively free the enumerator:
        c4queryenum_free(e);
        if (error.code != 0)
            throwError(env, error);
    }
    return result;
}

static jbyteArray toJByteArray(JNIEnv *env, const C4KeyReader &r) {
    C4SliceResult json = c4key_toJSON(&r);
    jbyteArray result = NULL;
    if (json.buf)
        result = toJByteArray(env, json);
    c4slice_free(json);
    return result;
}

JNIEXPORT jbyteArray JNICALL Java_com_couchbase_cbforest_QueryIterator_keyJSON
(JNIEnv *env, jclass clazz, jlong handle)
{
    if (!handle)
        return NULL;
    return toJByteArray(env, ((C4QueryEnumerator*)handle)->key);
}

JNIEXPORT jbyteArray JNICALL Java_com_couchbase_cbforest_QueryIterator_valueJSON
(JNIEnv *env, jclass clazz, jlong handle)
{
    if (!handle)
        return NULL;
    return toJByteArray(env, ((C4QueryEnumerator*)handle)->value);
}

JNIEXPORT jstring JNICALL Java_com_couchbase_cbforest_QueryIterator_docID
(JNIEnv *env, jclass clazz, jlong handle)
{
    if (!handle)
        return NULL;
    return toJString(env, ((C4QueryEnumerator*)handle)->docID);
}

JNIEXPORT jlong JNICALL Java_com_couchbase_cbforest_QueryIterator_sequence
        (JNIEnv *env, jclass clazz, jlong handle)
{
    if (!handle)
        return 0;
    return ((C4QueryEnumerator*)handle)->docSequence;
}

JNIEXPORT jint JNICALL Java_com_couchbase_cbforest_QueryIterator_fullTextID
  (JNIEnv *env, jclass clazz, jlong handle)
{
    if (!handle)
        return 0;
    return ((C4QueryEnumerator*)handle)->fullTextID;
}

JNIEXPORT jintArray JNICALL Java_com_couchbase_cbforest_QueryIterator_fullTextTerms
  (JNIEnv *env, jclass clazz, jlong handle)
{
    if (!handle)
        return NULL;
    auto e = (C4QueryEnumerator*)handle;
    jintArray jterms = env->NewIntArray(3 * e->fullTextTermCount);
    if (!jterms)
        return NULL;
    jboolean isCopy;
    jint *term = env->GetIntArrayElements(jterms, &isCopy);
    for (uint32_t i = 0; i < e->fullTextTermCount; i++) {
        auto &src = e->fullTextTerms[i];
        term[0] = src.termIndex;
        term[1] = src.start;
        term[2] = src.length;
        term += 3;
    }
    return jterms;
}

JNIEXPORT jdoubleArray JNICALL Java_com_couchbase_cbforest_QueryIterator_geoBoundingBox
  (JNIEnv *env, jclass clazz, jlong handle)
{
    if (!handle)
        return NULL;
    auto e = (C4QueryEnumerator*)handle;
    jdoubleArray jbox = env->NewDoubleArray(4);
    if (!jbox)
        return NULL;
    jboolean isCopy;
    jdouble *bp = env->GetDoubleArrayElements(jbox, &isCopy);
    bp[0] = e->geoBBox.xmin;
    bp[1] = e->geoBBox.ymin;
    bp[2] = e->geoBBox.xmax;
    bp[3] = e->geoBBox.ymax;
    return jbox;
}

JNIEXPORT jbyteArray JNICALL Java_com_couchbase_cbforest_QueryIterator_geoJSON
  (JNIEnv *env, jclass clazz, jlong handle)
{
    if (!handle)
        return NULL;
    auto e = (C4QueryEnumerator*)handle;
    return toJByteArray(env, e->geoJSON);
}

JNIEXPORT void JNICALL Java_com_couchbase_cbforest_QueryIterator_free
(JNIEnv *env, jclass clazz, jlong handle)
{
    C4QueryEnumerator *e = (C4QueryEnumerator*)handle;
    c4queryenum_free(e);
}


JNIEXPORT jstring JNICALL Java_com_couchbase_cbforest_FullTextResult_getFullText
  (JNIEnv *env, jclass clazz, jlong viewHandle, jstring jdocID, jlong sequence, jint fullTextID)
{
    if (!viewHandle)
        return NULL;
    jstringSlice docID(env, jdocID);
    C4Error err;
    C4SliceResult text = c4view_fullTextMatched((C4View*)viewHandle, docID, sequence, fullTextID,
                                                &err);
    if (!text.buf) {
        throwError(env, err);
        return NULL;
    }
    jstring result = toJString(env, text);
    free((void*)text.buf);
    return result;
}