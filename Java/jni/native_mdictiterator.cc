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
#include "com_couchbase_litecore_fleece_MDictIterator.h"

using namespace fleece;
using namespace fleeceapi;
using namespace litecore::jni;

// ----------------------------------------------------------------------------
// MDictIterator Helper method
// ----------------------------------------------------------------------------

static jstring key(JNIEnv *env, JMDictIterator *itr) {
    slice s = itr->key();
    jstring ret = toJStringFromSlice(env, s);
    return ret;
}

// ----------------------------------------------------------------------------
// MDictIterator JNI bindings
// ----------------------------------------------------------------------------
/*
 * Class:     com_couchbase_litecore_fleece_MDictIterator
 * Method:    free
 * Signature: (J)V
 */
JNIEXPORT void JNICALL
Java_com_couchbase_litecore_fleece_MDictIterator_free(JNIEnv *env, jclass clazz, jlong jitr) {
    JMDictIterator *itr = (JMDictIterator *) jitr;
    if (itr != NULL)
        delete itr;
}

/*
 * Class:     com_couchbase_litecore_fleece_MDictIterator
 * Method:    init
 * Signature: (J)J
 */
JNIEXPORT jlong JNICALL
Java_com_couchbase_litecore_fleece_MDictIterator_init(JNIEnv *env, jclass clazz, jlong jdict) {
    return (jlong) new JMDictIterator(*(JMDict *) jdict);
}

/*
 * Class:     com_couchbase_litecore_fleece_MDictIterator
 * Method:    key
 * Signature: (J)Ljava/lang/Object;
 */
JNIEXPORT jstring JNICALL
Java_com_couchbase_litecore_fleece_MDictIterator_key(JNIEnv *env, jclass clazz, jlong jitr) {
    return key(env, (JMDictIterator *) jitr);
}

/*
 * Class:     com_couchbase_litecore_fleece_MDictIterator
 * Method:    value
 * Signature: (J)Ljava/lang/Object;
 */
JNIEXPORT jobject JNICALL
Java_com_couchbase_litecore_fleece_MDictIterator_value(JNIEnv *env, jclass clazz, jlong jitr) {
    JMDictIterator *itr = (JMDictIterator *) jitr;
    JNative ref = itr->nativeValue();
    return ref->native();
}

/*
 * Class:     com_couchbase_litecore_fleece_MDictIterator
 * Method:    next
 * Signature: (J)Ljava/lang/String;
 */
JNIEXPORT jboolean JNICALL
Java_com_couchbase_litecore_fleece_MDictIterator_next(JNIEnv *env, jclass clazz, jlong jitr) {
    try {
        auto &iter = *(JMDictIterator *) jitr;
        ++iter;                 // throws if iterating past end
        return (bool) iter;
    } catchError(nullptr)
    return false;
}
