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
#include "com_couchbase_litecore_fleece_MCollection.h"

using namespace fleece;
using namespace fleeceapi;
using namespace litecore::jni;

// ----------------------------------------------------------------------------
// MCollection JNI bindings
// ----------------------------------------------------------------------------

/*
 * Class:     com_couchbase_litecore_fleece_MCollection
 * Method:    isMutable
 * Signature: (J)Z
 */
JNIEXPORT jboolean
JNICALL
Java_com_couchbase_litecore_fleece_MCollection_isMutable
        (JNIEnv *env, jclass clazz, jlong jmcoll) {
    return ((JMCollection *) jmcoll)->isMutable();
}

/*
 * Class:     com_couchbase_litecore_fleece_MCollection
 * Method:    isMutated
 * Signature: (J)Z
 */
JNIEXPORT jboolean
JNICALL
Java_com_couchbase_litecore_fleece_MCollection_isMutated
        (JNIEnv *env, jclass clazz, jlong jmcoll) {
    return ((JMCollection *) jmcoll)->isMutated();
}

/*
 * Class:     com_couchbase_litecore_fleece_MCollection
 * Method:    mutableChildren
 * Signature: (J)Z
 */
JNIEXPORT jboolean
JNICALL Java_com_couchbase_litecore_fleece_MCollection_mutableChildren
        (JNIEnv *env, jclass clazz, jlong jmcoll) {
    return ((JMCollection *) jmcoll)->mutableChildren();
}

/*
 * Class:     com_couchbase_litecore_fleece_MCollection
 * Method:    setMutableChildren
 * Signature: (JZ)V
 */
JNIEXPORT void JNICALL
Java_com_couchbase_litecore_fleece_MCollection_setMutableChildren
        (JNIEnv *env, jclass clazz, jlong jmcoll, jboolean jm) {
    ((JMCollection *) jmcoll)->setMutableChildren(jm);
}

/*
 * Class:     com_couchbase_litecore_fleece_MCollection
 * Method:    context
 * Signature: (J)J
 */
JNIEXPORT jlong
JNICALL
Java_com_couchbase_litecore_fleece_MCollection_context
        (JNIEnv *env, jclass clazz, jlong jmcoll) {
    return (jlong) ((JMCollection *) jmcoll)->context();
}

/*
 * Class:     com_couchbase_litecore_fleece_MCollection
 * Method:    parent
 * Signature: (J)J
 */
JNIEXPORT jlong
JNICALL
Java_com_couchbase_litecore_fleece_MCollection_parent
        (JNIEnv *env, jclass clazz, jlong jmcoll) {
    return (jlong) ((JMCollection *) jmcoll)->parent();
}