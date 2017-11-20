//
// Created by hideki on 11/15/17.
//

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
    if (context != NULL) delete context;
}

/*
 * Class:     com_couchbase_litecore_fleece_MContext
 * Method:    sharedKeys
 * Signature: (J)J
 */
JNIEXPORT jlong JNICALL Java_com_couchbase_litecore_fleece_MContext_sharedKeys(JNIEnv *env, jclass clazz, jlong jmcontext){
    return (jlong)((JMContext *) jmcontext)->sharedKeys();
}

/*
 * Class:     com_couchbase_litecore_fleece_MContext
 * Method:    setNative
 * Signature: (JLjava/lang/Object;)V
 */
JNIEXPORT void JNICALL Java_com_couchbase_litecore_fleece_MContext_setNative(JNIEnv *env, jclass clazz, jlong jmcontext, jobject jobj){
    ((JMContext *) jmcontext)->setJNative(env, jobj);
}
/*
 * Class:     com_couchbase_litecore_fleece_MContext
 * Method:    getNative
 * Signature: (J)Ljava/lang/Object;
 */
JNIEXPORT jobject JNICALL Java_com_couchbase_litecore_fleece_MContext_getNative(JNIEnv *env, jclass clazz, jlong jmcontext){
    return ((JMContext *) jmcontext)->getJNative();
}