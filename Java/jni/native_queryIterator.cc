//
//  native_queryIterator.cc
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 9/17/15.
//  Copyright (c) 2015-2016 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#include "com_couchbase_litecore_QueryIterator.h"
#include "com_couchbase_litecore_FullTextResult.h"
#include "native_glue.hh"
#include "Collatable.hh"
#include "c4View.h"


using namespace litecore;
using namespace litecore::jni;


static jfieldID kHandleField;


bool litecore::jni::initQueryIterator(JNIEnv *env) {
    jclass queryIterClass = env->FindClass("com/couchbase/litecore/QueryIterator");
    if (!queryIterClass)
        return false;
    kHandleField = env->GetFieldID(queryIterClass, "_handle", "J");
    return (kHandleField != nullptr);
}


JNIEXPORT jboolean JNICALL Java_com_couchbase_litecore_QueryIterator_next
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
    jbyteArray result = nullptr;
    if (json.buf)
        result = toJByteArray(env, json);
    c4slice_free(json);
    return result;
}

JNIEXPORT jbyteArray JNICALL Java_com_couchbase_litecore_QueryIterator_keyJSON
(JNIEnv *env, jclass clazz, jlong handle)
{
    if (!handle)
        return nullptr;
    return toJByteArray(env, ((C4QueryEnumerator*)handle)->key);
}

JNIEXPORT jbyteArray JNICALL Java_com_couchbase_litecore_QueryIterator_valueJSON
(JNIEnv *env, jclass clazz, jlong handle)
{
    if (!handle)
        return nullptr;
    return toJByteArray(env, ((C4QueryEnumerator*)handle)->value);
}

JNIEXPORT jstring JNICALL Java_com_couchbase_litecore_QueryIterator_docID
(JNIEnv *env, jclass clazz, jlong handle)
{
    if (!handle)
        return nullptr;
    return toJString(env, ((C4QueryEnumerator*)handle)->docID);
}

JNIEXPORT jlong JNICALL Java_com_couchbase_litecore_QueryIterator_sequence
        (JNIEnv *env, jclass clazz, jlong handle)
{
    if (!handle)
        return 0;
    return ((C4QueryEnumerator*)handle)->docSequence;
}

JNIEXPORT void JNICALL Java_com_couchbase_litecore_QueryIterator_free
(JNIEnv *env, jclass clazz, jlong handle)
{
    C4QueryEnumerator *e = (C4QueryEnumerator*)handle;
    c4queryenum_free(e);
}
