//
//  native_view.cc
//  CBForest
//
//  Created by Jens Alfke on 9/17/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//

#include "com_couchbase_cbforest_View.h"
#include "native_glue.hh"
#include "c4View.h"
#include <algorithm>


using namespace forestdb::jni;


#pragma mark - DATABASE:


static jfieldID kHandleField;


static inline C4View* getViewHandle(JNIEnv *env, jobject self) {
    return (C4View*)env->GetLongField(self, kHandleField);
}

bool forestdb::jni::initView(JNIEnv *env) {
    jclass viewClass = env->FindClass("com/couchbase/cbforest/View");
    if (!viewClass)
        return false;
    kHandleField = env->GetFieldID(viewClass, "_handle", "L");
    return (kHandleField != NULL);
}


JNIEXPORT jlong JNICALL Java_com_couchbase_cbforest_View__1open
  (JNIEnv *env, jobject self, jstring jpath, jlong dbHandle, jstring jname, jstring jversion)
{
    jstringSlice path(env, jpath), name(env, jname), version(env, jversion);
    C4Error error;
    C4View *view = c4view_open((C4Database*)dbHandle, path, name, version, &error);
    if (!view)
        throwError(env, error);
    return (jlong)view;
}


JNIEXPORT void JNICALL Java_com_couchbase_cbforest_View_close
  (JNIEnv *env, jobject self)
{
    C4View* view = getViewHandle(env, self);
    env->SetLongField(self, kHandleField, 0);
    C4Error error;
    if (!c4view_close(view, &error))
        throwError(env, error);
}


JNIEXPORT void JNICALL Java_com_couchbase_cbforest_View_eraseIndex
  (JNIEnv *env, jobject self)
{
    C4Error error;
    if (!c4view_eraseIndex(getViewHandle(env, self), &error))
        throwError(env, error);
}


JNIEXPORT void JNICALL Java_com_couchbase_cbforest_View_delete
  (JNIEnv *env, jobject self)
{
    C4View* view = getViewHandle(env, self);
    env->SetLongField(self, kHandleField, 0);
    C4Error error;
    if (!c4view_delete(view, &error))
        throwError(env, error);
}


JNIEXPORT jlong JNICALL Java_com_couchbase_cbforest_View_getTotalRows
  (JNIEnv *env, jobject self)
{
    return c4view_getTotalRows(getViewHandle(env, self));
}


JNIEXPORT jlong JNICALL Java_com_couchbase_cbforest_View_getLastSequenceIndexed
  (JNIEnv *env, jobject self)
{
    return c4view_getLastSequenceIndexed(getViewHandle(env, self));
}


JNIEXPORT jlong JNICALL Java_com_couchbase_cbforest_View_getLastSequenceChangedAt
  (JNIEnv *env, jobject self)
{
    return c4view_getLastSequenceChangedAt(getViewHandle(env, self));
}


JNIEXPORT jlong JNICALL Java_com_couchbase_cbforest_View_query__JJJZZZJJLjava_lang_String_2Ljava_lang_String_2
  (JNIEnv *env, jclass clazz, jlong viewHandle,
   jlong skip, jlong limit,
   jboolean descending, jboolean inclusiveStart, jboolean inclusiveEnd,
   jlong startKey, jlong endKey, jstring jstartKeyDocID, jstring jendKeyDocID)
{
    jstringSlice startKeyDocID(env, jstartKeyDocID), endKeyDocID(env, jendKeyDocID);
    C4QueryOptions options = {
        (uint64_t)std::max(skip, 0ll),
        (uint64_t)std::max(limit, 0ll),
        (bool)descending,
        (bool)inclusiveStart,
        (bool)inclusiveEnd,
        (C4Key*)startKey,
        (C4Key*)endKey,
        startKeyDocID,
        endKeyDocID
    };
    C4Error error;
    C4QueryEnumerator *e = c4view_query((C4View*)viewHandle, &options, &error);
    if (!e)
        throwError(env, error);
    return (jlong)e;
}


JNIEXPORT jlong JNICALL Java_com_couchbase_cbforest_View_query__JJJZZZ_3J
(JNIEnv *env, jclass clazz, jlong viewHandle,
 jlong skip, jlong limit,
 jboolean descending, jboolean inclusiveStart, jboolean inclusiveEnd,
 jlongArray jkeys)
{
    size_t keyCount = env->GetArrayLength(jkeys);
    jboolean isCopy;
    auto keys = env->GetLongArrayElements(jkeys, &isCopy);
    C4QueryOptions options = {
        (uint64_t)std::max(skip, 0ll),
        (uint64_t)std::max(limit, 0ll),
        (bool)descending,
        (bool)inclusiveStart,
        (bool)inclusiveEnd,
        NULL,
        NULL,
        kC4SliceNull,
        kC4SliceNull,
        (const C4Key**)keys,
        keyCount
    };
    C4Error error;
    C4QueryEnumerator *e = c4view_query((C4View*)viewHandle, &options, &error);
    env->ReleaseLongArrayElements(jkeys, keys, JNI_ABORT);
    if (!e)
        throwError(env, error);
    return (jlong)e;
}


#pragma mark - KEYS:


JNIEXPORT jlong JNICALL Java_com_couchbase_cbforest_View_newKey
  (JNIEnv *env, jclass clazz)
{
    return (jlong)c4key_new();
}

JNIEXPORT void JNICALL Java_com_couchbase_cbforest_View_freeKey
  (JNIEnv *env, jclass clazz, jlong jkey)
{
    c4key_free((C4Key*)jkey);
}

JNIEXPORT void JNICALL Java_com_couchbase_cbforest_View_keyAddNull
  (JNIEnv *env, jclass clazz, jlong jkey)
{
    c4key_addNull((C4Key*)jkey);
}

JNIEXPORT void JNICALL Java_com_couchbase_cbforest_View_keyAdd__JZ
  (JNIEnv *env, jclass clazz, jlong jkey, jboolean b)
{
    c4key_addBool((C4Key*)jkey, b);
}

JNIEXPORT void JNICALL Java_com_couchbase_cbforest_View_keyAdd__JD
  (JNIEnv *env, jclass clazz, jlong jkey, jdouble d)
{
    c4key_addNumber((C4Key*)jkey, d);
}

JNIEXPORT void JNICALL Java_com_couchbase_cbforest_View_keyAdd__JLjava_lang_String_2
  (JNIEnv *env, jclass clazz, jlong jkey, jstring s)
{
    jstringSlice str(env, s);
    c4key_addString((C4Key*)jkey, str);
}

JNIEXPORT void JNICALL Java_com_couchbase_cbforest_View_keyBeginArray
  (JNIEnv *env, jclass clazz, jlong jkey)
{
    c4key_beginArray((C4Key*)jkey);
}

JNIEXPORT void JNICALL Java_com_couchbase_cbforest_View_keyEndArray
  (JNIEnv *env, jclass clazz, jlong jkey)
{
    c4key_endArray((C4Key*)jkey);
}

JNIEXPORT void JNICALL Java_com_couchbase_cbforest_View_keyBeginMap
  (JNIEnv *env, jclass clazz, jlong jkey)
{
    c4key_beginMap((C4Key*)jkey);
}

JNIEXPORT void JNICALL Java_com_couchbase_cbforest_View_keyEndMap
  (JNIEnv *env, jclass clazz, jlong jkey)
{
    c4key_endMap((C4Key*)jkey);
}

