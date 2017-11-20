//
// Created by hideki on 11/10/17.
//

#include "native_mutable.hh"
#include "com_couchbase_litecore_fleece_MRoot.h"

using namespace fleece;
using namespace fleeceapi;
using namespace litecore::jni;

// ----------------------------------------------------------------------------
// MRoot JNI bindings
// ----------------------------------------------------------------------------
/*
 * Class:     com_couchbase_litecore_fleece_MRoot
 * Method:    toNative
 * Signature: (JJZ)Ljava/lang/Object;
 */
JNIEXPORT jobject JNICALL
Java_com_couchbase_litecore_fleece_MRoot_toNative(JNIEnv *env, jclass clazz, jlong jfleeceData,
                                                  jlong jsk, jboolean isMutable) {

    alloc_slice *fleeceData = (alloc_slice *) jfleeceData;
    FLSharedKeys sk = (FLSharedKeys) jsk;
    JNative ref = JMRoot::asNative(*fleeceData, sk, isMutable);
    return ref->native();
}

/*
 * Class:     com_couchbase_litecore_fleece_MRoot
 * Method:    initWithContext
 * Signature: (JJZ)J
 */
JNIEXPORT jlong JNICALL Java_com_couchbase_litecore_fleece_MRoot_initWithContext(JNIEnv *env, jclass clazz, jlong jcontext, jlong jvalue, jboolean isMutable){
    Value value((FLValue)jvalue);
    return (jlong) new JMRoot((MContext *)jcontext, value, isMutable);
}

/*
 * Class:     com_couchbase_litecore_fleece_MRoot
 * Method:    init
 * Signature: (JJZ)J
 */
JNIEXPORT jlong JNICALL
Java_com_couchbase_litecore_fleece_MRoot_init(JNIEnv *env, jclass clazz, jlong jfleeceData,
                                              jlong jsk, jboolean isMutable) {
    alloc_slice *fleeceData = (alloc_slice *) jfleeceData;
    FLSharedKeys sk = (FLSharedKeys) jsk;
    return (jlong) new JMRoot(*fleeceData, sk, isMutable);
}

/*
 * Class:     com_couchbase_litecore_fleece_MRoot
 * Method:    free
 * Signature: (J)V
 */
JNIEXPORT void JNICALL
Java_com_couchbase_litecore_fleece_MRoot_free(JNIEnv *env, jclass clazz, jlong jmroot) {
    delete (JMRoot *) jmroot;
}

/*
 * Class:     com_couchbase_litecore_fleece_MRoot
 * Method:    context
 * Signature: (J)J
 */
JNIEXPORT jlong JNICALL
Java_com_couchbase_litecore_fleece_MRoot_context(JNIEnv *env, jclass clazz, jlong jmroot) {
    return (jlong) ((JMRoot *) jmroot)->context();
}

/*
 * Class:     com_couchbase_litecore_fleece_MRoot
 * Method:    asNative
 * Signature: (J)Ljava/lang/Object;
 */
JNIEXPORT jobject JNICALL
Java_com_couchbase_litecore_fleece_MRoot_asNative(JNIEnv *env, jclass clazz, jlong jmroot) {
    JNative ref = ((JMRoot *) jmroot)->asNative();
    return ref->native();
}

/*
 * Class:     com_couchbase_litecore_fleece_MRoot
 * Method:    isMutated
 * Signature: (J)Z
 */
JNIEXPORT jboolean JNICALL
Java_com_couchbase_litecore_fleece_MRoot_isMutated(JNIEnv *env, jclass clazz, jlong jmroot) {
    return ((JMRoot *) jmroot)->isMutated();
}

/*
 * Class:     com_couchbase_litecore_fleece_MRoot
 * Method:    encodeTo
 * Signature: (JJ)V
 */
JNIEXPORT void JNICALL
Java_com_couchbase_litecore_fleece_MRoot_encodeTo(JNIEnv *env, jclass clazz, jlong jmroot,
                                                  jlong jenc) {
    Encoder* enc = (Encoder*)jenc;
    ((JMRoot *) jmroot)->encodeTo(*enc);
}

/*
 * Class:     com_couchbase_litecore_fleece_MRoot
 * Method:    encode
 * Signature: (J)J
 */
JNIEXPORT jlong JNICALL
Java_com_couchbase_litecore_fleece_MRoot_encode(JNIEnv *env, jclass clazz, jlong jmroot) {
    alloc_slice delta = ((JMRoot *) jmroot)->encode();
    return (jlong) new alloc_slice(delta);
}

/*
 * Class:     com_couchbase_litecore_fleece_MRoot
 * Method:    encodeDelta
 * Signature: (J)J
 */
JNIEXPORT jlong JNICALL
Java_com_couchbase_litecore_fleece_MRoot_encodeDelta(JNIEnv *env, jclass clazz, jlong jmroot) {
    alloc_slice delta = ((JMRoot *) jmroot)->encodeDelta();
    return (jlong) new alloc_slice(delta);
}