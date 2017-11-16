//
// Created by hideki on 11/8/17.
//

#include "native_mutable.hh"
#include "com_couchbase_litecore_fleece_MDict.h"

using namespace fleece;
using namespace fleeceapi;
using namespace litecore::jni;

// ----------------------------------------------------------------------------
// MDict JNI bindings
// ----------------------------------------------------------------------------

/*
 * Class:     com_couchbase_litecore_fleece_MDict
 * Method:    free
 * Signature: (J)V
 */
JNIEXPORT void JNICALL
Java_com_couchbase_litecore_fleece_MDict_free(JNIEnv *env, jclass clazz, jlong jmdict) {
    delete (JMDict *) jmdict;
}

/*
 * Class:     com_couchbase_litecore_fleece_MDict
 * Method:    init
 * Signature: ()J
 */
JNIEXPORT jlong JNICALL Java_com_couchbase_litecore_fleece_MDict_init(JNIEnv *env, jclass clazz) {
    return (jlong) new JMDict();
}

/*
 * Class:     com_couchbase_litecore_fleece_MDict
 * Method:    initInSlot
 * Signature: (JJJ)V
 */
JNIEXPORT void JNICALL
Java_com_couchbase_litecore_fleece_MDict_initInSlot(JNIEnv *env, jclass clazz, jlong jmdict,
                                                    jlong jmv, jlong jparent) {
    ((JMDict *) jmdict)->initInSlot((JMValue *) jmv, (JMCollection *) jparent);
}

/*
 * Class:     com_couchbase_litecore_fleece_MDict
 * Method:    initAsCopyOf
 * Signature: (JJZ)V
 */
JNIEXPORT void JNICALL
Java_com_couchbase_litecore_fleece_MDict_initAsCopyOf(JNIEnv *env, jclass clazz, jlong jmdict,
                                                      jlong jd, jboolean jisMutable) {
    ((JMDict *) jmdict)->initAsCopyOf(*((JMDict *) jd), jisMutable);
}

/*
 * Class:     com_couchbase_litecore_fleece_MDict
 * Method:    count
 * Signature: (J)J
 */
JNIEXPORT jlong JNICALL
Java_com_couchbase_litecore_fleece_MDict_count(JNIEnv *env, jclass clazz, jlong jmdict) {
    return ((JMDict *) jmdict)->count();
}
/*
 * Class:     com_couchbase_litecore_fleece_MDict
 * Method:    contains
 * Signature: (JLjava/lang/String;)Z
 */
JNIEXPORT jboolean JNICALL
Java_com_couchbase_litecore_fleece_MDict_contains(JNIEnv *env, jclass clazz, jlong jmdict,
                                                  jstring jkey) {
    jstringSlice key(env, jkey);
    return ((JMDict *) jmdict)->contains(key);
}

/*
 * Class:     com_couchbase_litecore_fleece_MDict
 * Method:    get
 * Signature: (JLjava/lang/String;)J
 */
JNIEXPORT jlong JNICALL
Java_com_couchbase_litecore_fleece_MDict_get(JNIEnv *env, jclass clazz, jlong jmdict,
                                             jstring jkey) {
    jstringSlice key(env, jkey);
    const JMValue &mval = ((JMDict *) jmdict)->get(key);
    return (jlong) &mval;
}

/*
 * Class:     com_couchbase_litecore_fleece_MDict
 * Method:    set
 * Signature: (JLjava/lang/String;J)Z
 */
JNIEXPORT jboolean JNICALL
Java_com_couchbase_litecore_fleece_MDict_set(JNIEnv *env, jclass clazz, jlong jmdict, jstring jkey,
                                             jlong jval) {
    jstringSlice key(env, jkey);
    return ((JMDict *) jmdict)->set(key, *((JMValue *) jval));
}

/*
 * Class:     com_couchbase_litecore_fleece_MDict
 * Method:    remove
 * Signature: (JLjava/lang/String;)Z
 */
JNIEXPORT jboolean JNICALL
Java_com_couchbase_litecore_fleece_MDict_remove(JNIEnv *env, jclass clazz, jlong jmdict,
                                                jstring jkey) {
    jstringSlice key(env, jkey);
    return ((JMDict *) jmdict)->remove(key);
}

/*
 * Class:     com_couchbase_litecore_fleece_MDict
 * Method:    clear
 * Signature: (J)Z
 */
JNIEXPORT jboolean JNICALL
Java_com_couchbase_litecore_fleece_MDict_clear(JNIEnv *env, jclass clazz, jlong jmdict) {
    return ((JMDict *) jmdict)->clear();
}

/*
 * Class:     com_couchbase_litecore_fleece_MDict
 * Method:    encodeTo
 * Signature: (JJ)V
 */
JNIEXPORT void JNICALL
Java_com_couchbase_litecore_fleece_MDict_encodeTo(JNIEnv *env, jclass clazz, jlong jmdict,
                                                  jlong jenc) {
    Encoder *enc = (Encoder *) jenc;
    ((JMDict *) jmdict)->encodeTo(*enc);
}

