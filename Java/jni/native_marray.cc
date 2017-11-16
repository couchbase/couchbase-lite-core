//
// Created by hideki on 11/10/17.
//

#include "native_mutable.hh"
#include "com_couchbase_litecore_fleece_MArray.h"

using namespace fleece;
using namespace fleeceapi;
using namespace litecore::jni;

// ----------------------------------------------------------------------------
// MDict JNI bindings
// ----------------------------------------------------------------------------

/*
 * Class:     com_couchbase_litecore_fleece_MArray
 * Method:    free
 * Signature: (J)V
 */
JNIEXPORT void JNICALL
Java_com_couchbase_litecore_fleece_MArray_free(JNIEnv *env, jclass clazz, jlong jmarray) {
    delete (JMArray *) jmarray;
}

/*
 * Class:     com_couchbase_litecore_fleece_MArray
 * Method:    init
 * Signature: ()J
 */
JNIEXPORT jlong JNICALL
Java_com_couchbase_litecore_fleece_MArray_init(JNIEnv *env, jclass clazz) {
    return (jlong) new JMArray();
}

/*
 * Class:     com_couchbase_litecore_fleece_MArray
 * Method:    initInSlot
 * Signature: (JJJ)V
 */
JNIEXPORT void JNICALL
Java_com_couchbase_litecore_fleece_MArray_initInSlot(JNIEnv *env, jclass clazz, jlong jmarray,
                                                     jlong jmv, jlong jparent) {
    ((JMArray *) jmarray)->initInSlot((JMValue *) jmv, (JMCollection *) jparent);
}

/*
 * Class:     com_couchbase_litecore_fleece_MArray
 * Method:    initAsCopyOf
 * Signature: (JJZ)V
 */
JNIEXPORT void JNICALL
Java_com_couchbase_litecore_fleece_MArray_initAsCopyOf(JNIEnv *env, jclass clazz, jlong jmarray,
                                                       jlong ja, jboolean jisMutable) {
    ((JMArray *) jmarray)->initAsCopyOf(*((JMArray *) ja), jisMutable);
}

/*
 * Class:     com_couchbase_litecore_fleece_MArray
 * Method:    count
 * Signature: (J)J
 */
JNIEXPORT jlong JNICALL
Java_com_couchbase_litecore_fleece_MArray_count(JNIEnv *env, jclass clazz, jlong jmarray) {
    return ((JMArray *) jmarray)->count();
}

/*
 * Class:     com_couchbase_litecore_fleece_MArray
 * Method:    get
 * Signature: (JI)J
 */
JNIEXPORT jlong JNICALL
Java_com_couchbase_litecore_fleece_MArray_get(JNIEnv *env, jclass clazz, jlong jmarray, jint i) {
    const JMValue &mval = ((JMArray *) jmarray)->get(i);
    return (jlong) &mval;
}

/*
 * Class:     com_couchbase_litecore_fleece_MArray
 * Method:    set
 * Signature: (JILjava/lang/Object;)Z
 */
JNIEXPORT jboolean JNICALL
Java_com_couchbase_litecore_fleece_MArray_set(JNIEnv *env, jclass clazz, jlong jmarray, jint i,
                                              jobject jval) {
    return ((JMArray *) jmarray)->set(i, JNative(new JNIRef(env, jval)));
}

/*
 * Class:     com_couchbase_litecore_fleece_MArray
 * Method:    insert
 * Signature: (JILjava/lang/Object;)Z
 */
JNIEXPORT jboolean JNICALL
Java_com_couchbase_litecore_fleece_MArray_insert(JNIEnv *env, jclass clazz, jlong jmarray, jint i,
                                                 jobject jval) {
    return ((JMArray *) jmarray)->insert(i, JNative(new JNIRef(env, jval)));
}

/*
 * Class:     com_couchbase_litecore_fleece_MArray
 * Method:    append
 * Signature: (JLjava/lang/Object;)Z
 */
JNIEXPORT jboolean JNICALL
Java_com_couchbase_litecore_fleece_MArray_append(JNIEnv *env, jclass clazz, jlong jmarray,
                                                 jobject jval) {
    return ((JMArray *) jmarray)->append(JNative(new JNIRef(env, jval)));
}

/*
 * Class:     com_couchbase_litecore_fleece_MArray
 * Method:    remove
 * Signature: (JII)Z
 */
JNIEXPORT jboolean JNICALL
Java_com_couchbase_litecore_fleece_MArray_remove(JNIEnv *env, jclass clazz, jlong jmarray, jint ji,
                                                 jint jn) {
    return ((JMArray *) jmarray)->remove(ji, jn);
}

/*
 * Class:     com_couchbase_litecore_fleece_MArray
 * Method:    clear
 * Signature: (J)Z
 */
JNIEXPORT jboolean JNICALL
Java_com_couchbase_litecore_fleece_MArray_clear(JNIEnv *env, jclass clazz, jlong jmarray) {
    return ((JMArray *) jmarray)->clear();
}

/*
 * Class:     com_couchbase_litecore_fleece_MArray
 * Method:    encodeTo
 * Signature: (JJ)V
 */
JNIEXPORT void JNICALL
Java_com_couchbase_litecore_fleece_MArray_encodeTo(JNIEnv *env, jclass clazz, jlong jmarray,
                                                   jlong jenc) {
    Encoder *enc = (Encoder *) jenc;
    ((JMArray *) jmarray)->encodeTo(*enc);
}
