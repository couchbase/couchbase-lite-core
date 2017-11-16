//
// Created by hideki on 11/12/17.
//

#include "native_glue.hh"
#include "com_couchbase_litecore_fleece_AllocSlice.h"

using namespace fleece;
using namespace litecore::jni;

// ----------------------------------------------------------------------------
// AllocSlice
// ----------------------------------------------------------------------------

/*
 * Class:     com_couchbase_litecore_fleece_AllocSlice
 * Method:    init
 * Signature: ([B)J
 */
JNIEXPORT jlong JNICALL
Java_com_couchbase_litecore_fleece_AllocSlice_init(JNIEnv *env, jclass clazz, jbyteArray jvalue) {
    alloc_slice *ptr = new alloc_slice;
    *ptr = jbyteArraySlice::copy(env, jvalue);
    return (jlong) ptr;
}

/*
 * Class:     com_couchbase_litecore_fleece_AllocSlice
 * Method:    free
 * Signature: (J)V
 */
JNIEXPORT void JNICALL
Java_com_couchbase_litecore_fleece_AllocSlice_free(JNIEnv *env, jclass clazz, jlong jslice) {
    delete (alloc_slice *) jslice;
}

/*
 * Class:     com_couchbase_litecore_fleece_AllocSlice
 * Method:    getBuf
 * Signature: (J)[B
 */
JNIEXPORT jbyteArray JNICALL
Java_com_couchbase_litecore_fleece_AllocSlice_getBuf(JNIEnv *env, jclass clazz, jlong jslice) {
    alloc_slice *s = (alloc_slice *) jslice;
    return toJByteArray(env, {s->buf, s->size});
}

/*
 * Class:     com_couchbase_litecore_fleece_AllocSlice
 * Method:    getSize
 * Signature: (J)J
 */
JNIEXPORT jlong JNICALL
Java_com_couchbase_litecore_fleece_AllocSlice_getSize(JNIEnv *env, jclass clazz, jlong jslice) {
    return (jlong) ((alloc_slice *) jslice)->size;
}
