//
//  native_queryIterator.cc
//  CBForest
//
//  Created by Jens Alfke on 9/17/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//

#include "com_couchbase_cbforest_QueryIterator.h"
#include "native_glue.hh"
#include "Collatable.hh"
#include "c4View.h"


using namespace forestdb;
using namespace forestdb::jni;


static jfieldID kHandleField;


static inline C4QueryEnumerator* getEnum(JNIEnv *env, jobject self) {
    return (C4QueryEnumerator*)env->GetLongField(self, kHandleField);
}

bool forestdb::jni::initQueryIterator(JNIEnv *env) {
    jclass queryIterClass = env->FindClass("com/couchbase/cbforest/QueryIterator");
    if (!queryIterClass)
        return false;
    kHandleField = env->GetFieldID(queryIterClass, "_handle", "L");
    return (kHandleField != NULL);
}


JNIEXPORT jboolean JNICALL Java_com_couchbase_cbforest_QueryIterator_next
  (JNIEnv *env, jobject self)
{
    auto e = getEnum(env, self);
    if (!e)
        return false;
    C4Error error;
    jboolean result = c4queryenum_next(e, &error);
    if (!result) {
        // At end of iteration, proactively free the enumerator:
        Java_com_couchbase_cbforest_QueryIterator_free(env, self);
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
(JNIEnv *env, jobject self)
{
    return toJByteArray(env, getEnum(env, self)->key);
}

JNIEXPORT jbyteArray JNICALL Java_com_couchbase_cbforest_QueryIterator_valueJSON
(JNIEnv *env, jobject self)
{
    return toJByteArray(env, getEnum(env, self)->value);
}

JNIEXPORT jstring JNICALL Java_com_couchbase_cbforest_QueryIterator_docID
(JNIEnv *env, jobject self)
{
    return toJString(env, getEnum(env, self)->docID);
}

JNIEXPORT void JNICALL Java_com_couchbase_cbforest_QueryIterator_free
(JNIEnv *env, jobject self)
{
    C4QueryEnumerator *e = getEnum(env, self);
    env->SetLongField(self, kHandleField, 0);
    c4queryenum_free(e);
}
