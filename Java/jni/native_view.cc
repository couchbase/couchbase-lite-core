//
//  native_view.cc
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

#include "com_couchbase_cbforest_View.h"
#include "com_couchbase_cbforest_View_TextKey.h"
#include "native_glue.hh"
#include "c4View.h"
#include <algorithm>
#include <vector>


using namespace cbforest::jni;


#pragma mark - DATABASE:


static jfieldID kHandleField;

static inline C4View* getViewHandle(JNIEnv *env, jobject self) {
    return (C4View*)env->GetLongField(self, kHandleField);
}

bool cbforest::jni::initView(JNIEnv *env) {
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
    C4Error error;
    if (!c4view_close(view, &error))
        throwError(env, error);
}


JNIEXPORT void JNICALL Java_com_couchbase_cbforest_View_freeHandle
  (JNIEnv *env, jclass clazz, jlong handle)
{
    c4view_free((C4View*)handle);
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
        true, // rankFullText
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
    auto c4keys = handlesToVector<C4Key*>(env, jkeys);
    size_t keyCount = c4keys.size();

    C4QueryOptions options = {
        (uint64_t)std::max((long long)skip, 0ll),
        (uint64_t)std::max((long long)limit, 0ll),
        (bool)descending,
        (bool)inclusiveStart,
        (bool)inclusiveEnd,
        true, // rankFullText
        NULL,
        NULL,
        kC4SliceNull,
        kC4SliceNull,
        const_cast<const C4Key**>(c4keys.data()),
        keyCount
    };
    C4Error error;
    C4QueryEnumerator *e = c4view_query((C4View*)viewHandle, &options, &error);
    if (!e)
        throwError(env, error);
    return (jlong)e;
}


JNIEXPORT jlong JNICALL Java_com_couchbase_cbforest_View_query__JLjava_lang_String_2Ljava_lang_String_2Z
  (JNIEnv *env, jclass clazz, jlong viewHandle,
   jstring jqueryString, jstring jlanguageCode, jboolean ranked)
{
    jstringSlice queryString(env, jqueryString);
    jstringSlice languageCode(env, jlanguageCode);
    C4QueryOptions options = kC4DefaultQueryOptions;
    options.rankFullText = ranked;

    C4Error error;
    C4QueryEnumerator *e = c4view_fullTextQuery((C4View*)viewHandle, queryString, languageCode,
                                                &options, &error);
    if (!e)
        throwError(env, error);
    return (jlong)e;
}


JNIEXPORT jlong JNICALL Java_com_couchbase_cbforest_View_query__JDDDD
  (JNIEnv *env, jclass clazz, jlong viewHandle,
   jdouble xmin, jdouble ymin, jdouble xmax, jdouble ymax)
{
    C4GeoArea area = {xmin, ymin, xmax, ymax};
    C4Error error;
    C4QueryEnumerator *e = c4view_geoQuery((C4View*)viewHandle, area, &error);
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

JNIEXPORT jlong JNICALL Java_com_couchbase_cbforest_View_newFullTextKey
  (JNIEnv *env, jclass clazz, jstring jtext, jstring jlanguageCode)
{
    jstringSlice text(env, jtext);
    jstringSlice languageCode(env, jlanguageCode);
    return (jlong)c4key_newFullTextString(text, languageCode);
}

JNIEXPORT jlong JNICALL Java_com_couchbase_cbforest_View_newGeoKey
  (JNIEnv *env, jclass clazz, jbyteArray jgeoJSON,
   jdouble xmin, jdouble ymin, jdouble xmax, jdouble ymax)
{
    jbyteArraySlice geoJSON(env, jgeoJSON);
    C4GeoArea bbox = {xmin, ymin, xmax, ymax};
    return (jlong)c4key_newGeoJSON(geoJSON, bbox);
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

JNIEXPORT void JNICALL Java_com_couchbase_cbforest_View_00024TextKey_setDefaultLanguageCode
  (JNIEnv *env, jclass clazz, jstring jlanguageCode, jboolean ignoreDiacriticals)
{
    jstringSlice languageCode(env, jlanguageCode);
    c4key_setDefaultFullTextLanguage(languageCode, ignoreDiacriticals);
}
