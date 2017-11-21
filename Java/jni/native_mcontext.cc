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

#include "native_mutable.hh"
#include "com_couchbase_litecore_fleece_MContext.h"

using namespace fleece;
using namespace fleeceapi;
using namespace litecore::jni;

// ----------------------------------------------------------------------------
// MContext JNI bindings
// ----------------------------------------------------------------------------

/*
 * Class:     com_couchbase_litecore_fleece_MContext
 * Method:    init
 * Signature: (JJ)J
 */
JNIEXPORT jlong JNICALL
Java_com_couchbase_litecore_fleece_MContext_init(JNIEnv *env, jclass clazz, jlong jdata, jlong jsk) {
    alloc_slice* data = (alloc_slice *) jdata;
    FLSharedKeys sk = (FLSharedKeys) jsk;
    JMContext* context = new JMContext(*data, sk);
    context->retain();
    return (jlong) context;
}

/*
 * Class:     com_couchbase_litecore_fleece_MContext
 * Method:    free
 * Signature: (J)V
 */
JNIEXPORT void JNICALL
Java_com_couchbase_litecore_fleece_MContext_free(JNIEnv *env, jclass clazz, jlong jmcontext) {
    JMContext* context = (JMContext *) jmcontext;
    if (context != NULL)
        context->release();
}

/*
 * Class:     com_couchbase_litecore_fleece_MContext
 * Method:    sharedKeys
 * Signature: (J)J
 */
JNIEXPORT jlong JNICALL
Java_com_couchbase_litecore_fleece_MContext_sharedKeys(JNIEnv *env, jclass clazz, jlong jmcontext) {
    JMContext *mcontext = (JMContext *) jmcontext;
    if (mcontext != NULL)
        return (jlong) mcontext->sharedKeys();
    else
        return (jlong) 0L;
}

/*
 * Class:     com_couchbase_litecore_fleece_MContext
 * Method:    setNative
 * Signature: (JLjava/lang/Object;)V
 */
JNIEXPORT void JNICALL
Java_com_couchbase_litecore_fleece_MContext_setNative(JNIEnv *env, jclass clazz, jlong jmcontext,
                                                      jobject jobj) {
    JMContext *mcontext = (JMContext *) jmcontext;
    if (mcontext != NULL)
        mcontext->setJNative(env, jobj);
}
/*
 * Class:     com_couchbase_litecore_fleece_MContext
 * Method:    getNative
 * Signature: (J)Ljava/lang/Object;
 */
JNIEXPORT jobject JNICALL Java_com_couchbase_litecore_fleece_MContext_getNative(JNIEnv *env, jclass clazz, jlong jmcontext){
    JMContext *mcontext = (JMContext *) jmcontext;
    if (mcontext != NULL)
        return (jobject) mcontext->getJNative();
    else
        return (jobject) NULL;
}