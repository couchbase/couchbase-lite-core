//
// Created by hideki on 11/11/17.
//

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