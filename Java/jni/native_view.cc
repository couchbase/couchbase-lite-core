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
#include <vector>


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
    kHandleField = env->GetFieldID(viewClass, "_handle", "J");
    return (kHandleField != NULL);
}

//////// VIEWS:

JNIEXPORT jlong JNICALL Java_com_couchbase_cbforest_View__1open
  (JNIEnv *env, jobject self, jlong dbHandle, jstring jpath,
   jint flags, jint encryptionAlg, jbyteArray encryptionKey,
   jstring jname, jstring jversion)
{
    jstringSlice path(env, jpath), name(env, jname), version(env, jversion);
    C4EncryptionKey key;
    if (!getEncryptionKey(env, encryptionAlg, encryptionKey, &key))
        return 0;
    C4Error error;
    C4View *view = c4view_open((C4Database*)dbHandle, path, name, version,
                               (C4DatabaseFlags)flags, &key, &error);
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


JNIEXPORT void JNICALL Java_com_couchbase_cbforest_View_rekey
(JNIEnv *env, jobject self, jint encryptionAlg, jbyteArray encryptionKey){
    C4EncryptionKey key;
    if (!getEncryptionKey(env, encryptionAlg, encryptionKey, &key))
        return;

    auto view = getViewHandle(env, self);
    if (view) {
        C4Error error;
        if(!c4view_rekey(view, &key, &error))
            throwError(env, error);
    }
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

//////// INDEXING:

JNIEXPORT jlong JNICALL Java_com_couchbase_cbforest_View_beginIndex
        (JNIEnv *env, jobject self, jlong dbHandle, jlong viewHandle)
{
    C4View* view = (C4View*)viewHandle;
    C4Error error;
    C4Indexer* indexer = c4indexer_begin((C4Database*)dbHandle, &view, 1, &error);
    if (!indexer)
        throwError(env, error);
    return (jlong)indexer;
}

JNIEXPORT jlong JNICALL Java_com_couchbase_cbforest_View_enumerateDocuments
        (JNIEnv *env, jobject self, jlong indexerHandle){
    C4Error error;
    C4DocEnumerator* e = c4indexer_enumerateDocuments((C4Indexer*)indexerHandle, &error);
    if(!e)
        throwError(env, error);
    return (jlong)e;
}

JNIEXPORT void JNICALL Java_com_couchbase_cbforest_View_emit(JNIEnv *env, jobject self, jlong indexerHandle, jlong documentHandler, jlongArray jkeys, jobjectArray jvalues)
{
    C4Indexer* indexer = (C4Indexer*)indexerHandle;
    C4Document* doc = (C4Document*)documentHandler;
    size_t count = env->GetArrayLength(jkeys);
    jlong *keys   = env->GetLongArrayElements(jkeys, NULL);
    std::vector<C4Key*> c4keys(count);
    std::vector<C4Slice> c4values(count);
    std::vector<jbyteArraySlice> valueBufs;
    for(int i = 0; i < count; i++) {
        c4keys[i] = (C4Key*)keys[i];
        jbyteArray jvalue = (jbyteArray) env->GetObjectArrayElement(jvalues, i);
        if (jvalue) {
            valueBufs.push_back(jbyteArraySlice(env, jvalue));
            c4values[i] = valueBufs.back();
        } else {
            c4values[i] = kC4SliceNull;
        }
    }

    C4Error error;
    bool result = c4indexer_emit(indexer, doc, 0, (unsigned)count,
                                 c4keys.data(), c4values.data(), &error);

    for(int i = 0; i < count; i++)
        c4key_free(c4keys[i]);
    env->ReleaseLongArrayElements(jkeys, keys, JNI_ABORT);

    if(!result)
        throwError(env, error);
}

JNIEXPORT void JNICALL Java_com_couchbase_cbforest_View_endIndex
        (JNIEnv *env, jobject self, jlong indexerHandle, jboolean commit)
{
    C4Error error;
    if(!c4indexer_end((C4Indexer *)indexerHandle, commit, &error))
        throwError(env, error);
}

//////// QUERYING:
JNIEXPORT jlong JNICALL Java_com_couchbase_cbforest_View_query__J
        (JNIEnv *env, jclass clazz, jlong viewHandle)
{
    C4Error error;
    C4QueryEnumerator *e = c4view_query((C4View*)viewHandle, NULL, &error);
    if (!e)
        throwError(env, error);
    return (jlong)e;
}

JNIEXPORT jlong JNICALL Java_com_couchbase_cbforest_View_query__JJJZZZJJLjava_lang_String_2Ljava_lang_String_2
  (JNIEnv *env, jclass clazz, jlong viewHandle,
   jlong skip, jlong limit,
   jboolean descending, jboolean inclusiveStart, jboolean inclusiveEnd,
   jlong startKey, jlong endKey, jstring jstartKeyDocID, jstring jendKeyDocID)
{
    jstringSlice startKeyDocID(env, jstartKeyDocID), endKeyDocID(env, jendKeyDocID);
    C4QueryOptions options = {
        (uint64_t)std::max((long long)skip, 0ll),
        (uint64_t)std::max((long long)limit, 0ll),
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
    std::vector<C4Key*> c4keys(keyCount);
    for(int i = 0; i < keyCount; i++){
        c4keys[i]   = (C4Key *)keys[i];
    }
    C4QueryOptions options = {
        (uint64_t)std::max((long long)skip, 0ll),
        (uint64_t)std::max((long long)limit, 0ll),
        (bool)descending,
        (bool)inclusiveStart,
        (bool)inclusiveEnd,
        NULL,
        NULL,
        kC4SliceNull,
        kC4SliceNull,
        (const C4Key **)c4keys.data(),
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

JNIEXPORT jlong JNICALL Java_com_couchbase_cbforest_View_keyReader(JNIEnv *env, jclass clazz, jlong jkey)
{
    return (jlong)c4key_newReader((C4Key*)jkey);
}

JNIEXPORT jstring JNICALL Java_com_couchbase_cbforest_View_keyToJSON(JNIEnv *env, jclass clazz, jlong jkey)
{
    C4KeyReader reader = c4key_read((C4Key*)jkey);
    C4SliceResult dump = c4key_toJSON(&reader);
    jstring result = toJString(env, dump);
    c4slice_free(dump);
    return result;
}

JNIEXPORT jint JNICALL Java_com_couchbase_cbforest_View_keyPeek(JNIEnv *env, jclass clazz, jlong jreader){
    return (jint)c4key_peek((C4KeyReader*)jreader);
}


JNIEXPORT void JNICALL Java_com_couchbase_cbforest_View_keySkipToken(JNIEnv *env, jclass clazz, jlong jreader){
    c4key_skipToken((C4KeyReader*)jreader);
}


JNIEXPORT jboolean JNICALL Java_com_couchbase_cbforest_View_keyReadBool(JNIEnv *env, jclass clazz, jlong jreader){
    return (jboolean)c4key_readBool((C4KeyReader*)jreader);
}


JNIEXPORT jdouble JNICALL Java_com_couchbase_cbforest_View_keyReadNumber(JNIEnv *env, jclass clazz, jlong jreader){
    return (jdouble)c4key_readNumber((C4KeyReader*)jreader);
}


JNIEXPORT jstring JNICALL Java_com_couchbase_cbforest_View_keyReadString(JNIEnv *env, jclass clazz, jlong jreader){
    C4SliceResult dump = c4key_readString((C4KeyReader*)jreader);
    jstring result = toJString(env, dump);
    c4slice_free(dump);
    return result;
}

JNIEXPORT void JNICALL Java_com_couchbase_cbforest_View_freeKeyReader(JNIEnv *env, jclass clazz, jlong jreader){
    if(jreader != 0) c4key_freeReader((C4KeyReader*)jreader);
}
