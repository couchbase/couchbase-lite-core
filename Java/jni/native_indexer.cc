//
//  native_indexer.cc
//  CBForest
//
//  Created by Jens Alfke on 12/18/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#include "com_couchbase_cbforest_Indexer.h"
#include "native_glue.hh"
#include "c4View.h"
#include <algorithm>
#include <vector>


using namespace cbforest::jni;


JNIEXPORT jlong JNICALL Java_com_couchbase_cbforest_Indexer_beginIndex
(JNIEnv *env, jclass clazz, jlong dbHandle, jlongArray viewHandles)
{
    auto c4views = handlesToVector<C4View*>(env, viewHandles);
    C4Error error;
    C4Indexer* indexer = c4indexer_begin((C4Database*)dbHandle, c4views.data(), c4views.size(), &error);
    if (!indexer)
        throwError(env, error);
    return (jlong)indexer;
}


JNIEXPORT void JNICALL Java_com_couchbase_cbforest_Indexer_triggerOnView
  (JNIEnv *env, jclass clazz, jlong indexerHandle, jlong viewHandle)
{
    auto indexer = (C4Indexer*)indexerHandle;
    auto view = (C4View*)viewHandle;
    c4indexer_triggerOnView(indexer, view);
}


JNIEXPORT jlong JNICALL Java_com_couchbase_cbforest_Indexer_iterateDocuments
(JNIEnv *env, jclass clazz, jlong indexerHandle)
{
    C4Error error;
    C4DocEnumerator* e = c4indexer_enumerateDocuments((C4Indexer*)indexerHandle, &error);
    if(!e && error.code != 0)
        throwError(env, error);
    return (jlong)e;
}


JNIEXPORT jboolean JNICALL Java_com_couchbase_cbforest_Indexer_shouldIndex
  (JNIEnv *env, jclass clazz, jlong indexerHandle, jlong docHandle, jint viewNumber)
{
    auto indexer = (C4Indexer*)indexerHandle;
    auto doc = (C4Document*)docHandle;
    return c4indexer_shouldIndexDocument(indexer, viewNumber, doc);
}


JNIEXPORT void JNICALL Java_com_couchbase_cbforest_Indexer_emit
(JNIEnv *env, jclass clazz,
 jlong indexerHandle, jlong documentHandler, jint viewNumber, jlongArray jkeys, jobjectArray jvalues)
{
    auto c4keys = handlesToVector<C4Key*>(env, jkeys);
    C4KeyValueList* kv = c4kv_new();
    int i = 0;
    for (auto key = c4keys.begin(); key != c4keys.end(); ++key, ++i) {
        jbyteArray jvalue = (jbyteArray) env->GetObjectArrayElement(jvalues, i);
        jbyteArraySlice value(env, jvalue);
        c4kv_add(kv, *key, value);
        c4key_free(*key);
    }

    C4Indexer* indexer = (C4Indexer*)indexerHandle;
    C4Document* doc = (C4Document*)documentHandler;
    C4Error error;
    if (!c4indexer_emitList(indexer, doc, viewNumber, kv, &error))
        throwError(env, error);

    c4kv_free(kv);
}


JNIEXPORT void JNICALL Java_com_couchbase_cbforest_Indexer_endIndex
(JNIEnv *env, jclass clazz, jlong indexerHandle, jboolean commit)
{
    C4Error error;
    if(!c4indexer_end((C4Indexer *)indexerHandle, commit, &error))
        throwError(env, error);
}

