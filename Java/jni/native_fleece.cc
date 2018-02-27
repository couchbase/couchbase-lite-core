//
// native_fleece.cc
//
// Copyright (c) 2017 Couchbase, Inc All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "native_glue.hh"
#include "com_couchbase_litecore_fleece_FLArray.h"
#include "com_couchbase_litecore_fleece_FLArrayIterator.h"
#include "com_couchbase_litecore_fleece_FLDict.h"
#include "com_couchbase_litecore_fleece_FLDictIterator.h"
#include "com_couchbase_litecore_fleece_FLValue.h"
#include "com_couchbase_litecore_fleece_FLEncoder.h"
#include "com_couchbase_litecore_fleece_FLSliceResult.h"

using namespace fleeceapi;
using namespace litecore;
using namespace litecore::jni;

// ----------------------------------------------------------------------------
// FLArray
// ----------------------------------------------------------------------------

/*
 * Class:     com_couchbase_litecore_fleece_FLArray
 * Method:    count
 * Signature: (J)J
 */
JNIEXPORT jlong JNICALL
Java_com_couchbase_litecore_fleece_FLArray_count(JNIEnv *env, jclass clazz, jlong jarray) {
    return (jlong) FLArray_Count((FLArray) jarray);
}

/*
 * Class:     com_couchbase_litecore_fleece_FLArray
 * Method:    get
 * Signature: (JJ)J
 */
JNIEXPORT jlong JNICALL
Java_com_couchbase_litecore_fleece_FLArray_get(JNIEnv *env, jclass clazz, jlong jarray,
                                               jlong jindex) {
    return (jlong) FLArray_Get((FLArray) jarray, (uint32_t) jindex);
}

// ----------------------------------------------------------------------------
// FLArrayIterator
// ----------------------------------------------------------------------------
/*
 * Class:     com_couchbase_litecore_fleece_FLArrayIterator
 * Method:    init
 * Signature: ()J
 */
JNIEXPORT jlong JNICALL
Java_com_couchbase_litecore_fleece_FLArrayIterator_init(JNIEnv *env, jclass clazz) {
    return (jlong) ::malloc(sizeof(FLArrayIterator));
}

/*
 * Class:     com_couchbase_litecore_fleece_FLArrayIterator
 * Method:    begin
 * Signature: (JJ)V
 */
JNIEXPORT void JNICALL
Java_com_couchbase_litecore_fleece_FLArrayIterator_begin(JNIEnv *env, jclass clazz, jlong jarray,
                                                         jlong jitr) {
    FLArrayIterator_Begin((FLArray) jarray, (FLArrayIterator *) jitr);
}

/*
 * Class:     com_couchbase_litecore_fleece_FLArrayIterator
 * Method:    getValue
 * Signature: (J)J
 */
JNIEXPORT jlong JNICALL
Java_com_couchbase_litecore_fleece_FLArrayIterator_getValue(JNIEnv *env, jclass clazz, jlong jitr) {
    return (jlong) FLArrayIterator_GetValue((FLArrayIterator *) jitr);
}

/*
 * Class:     com_couchbase_litecore_fleece_FLArrayIterator
 * Method:    getValueAt
 * Signature: (JI)J
 */
JNIEXPORT jlong JNICALL
Java_com_couchbase_litecore_fleece_FLArrayIterator_getValueAt(JNIEnv *env, jclass clazz, jlong jitr,
                                                              jint offset) {
    return (jlong) FLArrayIterator_GetValueAt((FLArrayIterator *) jitr, (uint32_t) offset);
}

/*
 * Class:     com_couchbase_litecore_fleece_FLArrayIterator
 * Method:    next
 * Signature: (J)Z
 */
JNIEXPORT jboolean JNICALL
Java_com_couchbase_litecore_fleece_FLArrayIterator_next(JNIEnv *env, jclass clazz, jlong jitr) {
    return (jboolean) FLArrayIterator_Next((FLArrayIterator *) jitr);
}

/*
 * Class:     com_couchbase_litecore_fleece_FLArrayIterator
 * Method:    free
 * Signature: (J)V
 */
JNIEXPORT void JNICALL
Java_com_couchbase_litecore_fleece_FLArrayIterator_free(JNIEnv *env, jclass clazz, jlong jitr) {
    ::free((FLArrayIterator *) jitr);
}

// ----------------------------------------------------------------------------
// FLDict
// ----------------------------------------------------------------------------

/*
 * Class:     com_couchbase_litecore_fleece_FLDict
 * Method:    count
 * Signature: (J)J
 */
JNIEXPORT jlong JNICALL
Java_com_couchbase_litecore_fleece_FLDict_count(JNIEnv *env, jclass clazz, jlong jdict) {
    return (jlong) FLDict_Count((FLDict) jdict);
}

/*
 * Class:     com_couchbase_litecore_fleece_FLDict
 * Method:    getSharedKey
 * Signature: (J[BJ)J
 */
JNIEXPORT jlong JNICALL
Java_com_couchbase_litecore_fleece_FLDict_getSharedKey(JNIEnv *env,
                                                       jclass clazz,
                                                       jlong jdict,
                                                       jbyteArray jkeystring,
                                                       jlong jsharedkeys) {
    jbyteArraySlice key(env, jkeystring, true);
    return (jlong) FLDict_GetSharedKey((FLDict) jdict,
                                       {((slice) key).buf, ((slice) key).size},
                                       (FLSharedKeys) jsharedkeys);
}
/*
 * Class:     com_couchbase_litecore_fleece_FLDict
 * Method:    getKeyString
 * Signature: (JI)Ljava/lang/String;
 */
JNIEXPORT jstring JNICALL Java_com_couchbase_litecore_fleece_FLDict_getKeyString(JNIEnv *env,
                                                                                 jclass clazz,
                                                                                 jlong jsharedKey,
                                                                                 jint jkeyCode) {
    FLError error = kFLNoError;
    FLString str = FLSharedKey_GetKeyString((FLSharedKeys) jsharedKey, (int) jkeyCode, &error);
    if (error != kFLNoError)
        throwError(env, {FleeceDomain, error});
    return toJString(env, {str.buf, str.size});
}

/*
 * Class:     com_couchbase_litecore_fleece_FLDict
 * Method:    getUnsorted
 * Signature: (J[B)J
 */
JNIEXPORT jlong JNICALL
Java_com_couchbase_litecore_fleece_FLDict_getUnsorted(JNIEnv *env,
                                                      jclass clazz,
                                                      jlong jdict,
                                                      jbyteArray jkeystring) {
    jbyteArraySlice key(env, jkeystring, true);
    return (jlong) FLDict_GetUnsorted((FLDict) jdict, {((slice) key).buf, ((slice) key).size});
}
// ----------------------------------------------------------------------------
// FLDictIterator
// ----------------------------------------------------------------------------
/*
 * Class:     com_couchbase_litecore_fleece_FLDictIterator
 * Method:    init
 * Signature: ()J
 */
JNIEXPORT jlong JNICALL
Java_com_couchbase_litecore_fleece_FLDictIterator_init(JNIEnv *env, jclass clazz) {
    return (jlong) ::malloc(sizeof(FLDictIterator));
}

/*
 * Class:     com_couchbase_litecore_fleece_FLDictIterator
 * Method:    begin
 * Signature: (JJ)V
 */
JNIEXPORT void JNICALL
Java_com_couchbase_litecore_fleece_FLDictIterator_begin(JNIEnv *env, jclass clazz, jlong jdict,
                                                        jlong jitr) {
    FLDictIterator_Begin((FLDict) jdict, (FLDictIterator *) jitr);
}

/*
 * Class:     com_couchbase_litecore_fleece_FLDictIterator
 * Method:    getKey
 * Signature: (J)J
 */
JNIEXPORT jlong JNICALL
Java_com_couchbase_litecore_fleece_FLDictIterator_getKey(JNIEnv *env, jclass clazz, jlong jitr) {
    return (jlong) FLDictIterator_GetKey((FLDictIterator *) jitr);
}

/*
 * Class:     com_couchbase_litecore_fleece_FLDictIterator
 * Method:    getValue
 * Signature: (J)J
 */
JNIEXPORT jlong JNICALL
Java_com_couchbase_litecore_fleece_FLDictIterator_getValue(JNIEnv *env, jclass clazz, jlong jitr) {
    return (jlong) FLDictIterator_GetValue((FLDictIterator *) jitr);
}

/*
 * Class:     com_couchbase_litecore_fleece_FLDictIterator
 * Method:    next
 * Signature: (J)Z
 */
JNIEXPORT jboolean JNICALL
Java_com_couchbase_litecore_fleece_FLDictIterator_next(JNIEnv *env, jclass clazz, jlong jitr) {
    return (jboolean) FLDictIterator_Next((FLDictIterator *) jitr);
}

/*
 * Class:     com_couchbase_litecore_fleece_FLDictIterator
 * Method:    free
 * Signature: (J)V
 */
JNIEXPORT void JNICALL
Java_com_couchbase_litecore_fleece_FLDictIterator_free(JNIEnv *env, jclass clazz, jlong jitr) {
    ::free((FLDictIterator *) jitr);
}

// ----------------------------------------------------------------------------
// FLValue
// ----------------------------------------------------------------------------

/*
 * Class:     com_couchbase_litecore_fleece_FLValue
 * Method:    fromData
 * Signature: (J)J
 */
JNIEXPORT jlong JNICALL
Java_com_couchbase_litecore_fleece_FLValue_fromData(JNIEnv *env, jclass clazz, jlong jflslice) {
    alloc_slice *s = (alloc_slice *) jflslice;
    return (jlong) FLValue_FromData({s->buf, s->size});
}

/*
 * Class:     com_couchbase_litecore_fleece_FLValue
 * Method:    fromTrustedData
 * Signature: (J)J
 */
JNIEXPORT jlong JNICALL
Java_com_couchbase_litecore_fleece_FLValue_fromTrustedData(JNIEnv *env, jclass clazz,
                                                           jbyteArray jdata) {
    jbyteArraySlice data(env, jdata, true);
    return (jlong) FLValue_FromTrustedData({((slice) data).buf, ((slice) data).size});
}
/*
 * Class:     com_couchbase_litecore_fleece_FLValue
 * Method:    fromTrustedData2
 * Signature: (J)J
 */
JNIEXPORT jlong JNICALL
Java_com_couchbase_litecore_fleece_FLValue_fromTrustedData2(JNIEnv *env, jclass clazz,
                                                            jlong jbody) {
    FLSlice *flSlice = (FLSlice *) jbody;
    return (jlong) FLValue_FromTrustedData(*flSlice);
}
/*
 * Class:     com_couchbase_litecore_fleece_FLValue
 * Method:    getType
 * Signature: (J)I
 */
JNIEXPORT jint JNICALL
Java_com_couchbase_litecore_fleece_FLValue_getType(JNIEnv *env, jclass clazz, jlong jvalue) {
    return FLValue_GetType((FLValue) jvalue);
}

/*
 * Class:     com_couchbase_litecore_fleece_FLValue
 * Method:    asBool
 * Signature: (J)Z
 */
JNIEXPORT jboolean JNICALL
Java_com_couchbase_litecore_fleece_FLValue_asBool(JNIEnv *env, jclass clazz, jlong jvalue) {
    return (jboolean) FLValue_AsBool((FLValue) jvalue);
}

/*
 * Class:     com_couchbase_litecore_fleece_FLValue
 * Method:    asUnsigned
 * Signature: (J)J
 */
JNIEXPORT jlong JNICALL
Java_com_couchbase_litecore_fleece_FLValue_asUnsigned(JNIEnv *env, jclass clazz, jlong jvalue) {
    return (jlong) FLValue_AsUnsigned((FLValue) jvalue);
}

/*
 * Class:     com_couchbase_litecore_fleece_FLValue
 * Method:    asInt
 * Signature: (J)J
 */
JNIEXPORT jlong JNICALL
Java_com_couchbase_litecore_fleece_FLValue_asInt(JNIEnv *env, jclass clazz, jlong jvalue) {
    return (jlong) FLValue_AsInt((FLValue) jvalue);
}

/*
 * Class:     com_couchbase_litecore_fleece_FLValue
 * Method:    asFloat
 * Signature: (J)F
 */
JNIEXPORT jfloat JNICALL
Java_com_couchbase_litecore_fleece_FLValue_asFloat(JNIEnv *env, jclass clazz, jlong jvalue) {
    return (jfloat) FLValue_AsFloat((FLValue) jvalue);
}

/*
 * Class:     com_couchbase_litecore_fleece_FLValue
 * Method:    asDouble
 * Signature: (J)D
 */
JNIEXPORT jdouble JNICALL
Java_com_couchbase_litecore_fleece_FLValue_asDouble(JNIEnv *env, jclass clazz, jlong jvalue) {
    return (jdouble) FLValue_AsDouble((FLValue) jvalue);
}

/*
 * Class:     com_couchbase_litecore_fleece_FLValue
 * Method:    asString
 * Signature: (J)Ljava/lang/String;
 */
JNIEXPORT jstring JNICALL
Java_com_couchbase_litecore_fleece_FLValue_asString(JNIEnv *env, jclass clazz, jlong jvalue) {
    FLString str = FLValue_AsString((FLValue) jvalue);
    return toJString(env, {str.buf, str.size});
}

/*
 * Class:     com_couchbase_litecore_fleece_FLValue
 * Method:    asData
 * Signature: (J)[B
 */
JNIEXPORT jbyteArray JNICALL
Java_com_couchbase_litecore_fleece_FLValue_asData(JNIEnv *env, jclass clazz, jlong jvalue) {
    FLSlice bytes = FLValue_AsData((FLValue) jvalue);
    return toJByteArray(env, {bytes.buf, bytes.size});
}

/*
 * Class:     com_couchbase_litecore_fleece_FLValue
 * Method:    asArray
 * Signature: (J)J
 */
JNIEXPORT jlong JNICALL
Java_com_couchbase_litecore_fleece_FLValue_asArray(JNIEnv *env, jclass clazz, jlong jvalue) {
    return (long) FLValue_AsArray((FLValue) jvalue);
}

/*
 * Class:     com_couchbase_litecore_fleece_FLValue
 * Method:    asDict
 * Signature: (J)J
 */
JNIEXPORT jlong JNICALL
Java_com_couchbase_litecore_fleece_FLValue_asDict(JNIEnv *env, jclass clazz, jlong jvalue) {
    return (long) FLValue_AsDict((FLValue) jvalue);
}

/*
 * Class:     com_couchbase_litecore_fleece_FLValue
 * Method:    isInteger
 * Signature: (J)Z
 */
JNIEXPORT jboolean JNICALL
Java_com_couchbase_litecore_fleece_FLValue_isInteger(JNIEnv *env, jclass clazz, jlong jvalue) {
    return (jboolean) FLValue_IsInteger((FLValue) jvalue);
}

/*
 * Class:     com_couchbase_litecore_fleece_FLValue
 * Method:    isDouble
 * Signature: (J)Z
 */
JNIEXPORT jboolean JNICALL
Java_com_couchbase_litecore_fleece_FLValue_isDouble(JNIEnv *env, jclass clazz, jlong jvalue) {
    return (jboolean) FLValue_IsDouble((FLValue) jvalue);
}
/*
 * Class:     com_couchbase_litecore_fleece_FLValue
 * Method:    isUnsigned
 * Signature: (J)Z
 */
JNIEXPORT jboolean JNICALL
Java_com_couchbase_litecore_fleece_FLValue_isUnsigned(JNIEnv *env, jclass clazz, jlong jvalue) {
    return (jboolean) FLValue_IsUnsigned((FLValue) jvalue);
}

/*
 * Class:     com_couchbase_litecore_fleece_FLValue
 * Method:    JSON5ToJSON
 * Signature: (Ljava/lang/String;)Ljava/lang/String;
 */
JNIEXPORT jstring JNICALL
Java_com_couchbase_litecore_fleece_FLValue_JSON5ToJSON(JNIEnv *env, jclass clazz, jstring jjson5) {
    jstringSlice json5(env, jjson5);
    FLError error = kFLNoError;
    FLStringResult json = FLJSON5_ToJSON({((slice) json5).buf, ((slice) json5).size}, &error);
    if (error != kFLNoError)
        throwError(env, {FleeceDomain, error});
    jstring res = toJString(env, {json.buf, json.size});
    FLSliceResult_Free(json);
    return res;
}
/*
 * Class:     com_couchbase_litecore_fleece_FLValue
 * Method:    toString
 * Signature: (J)Ljava/lang/String;
 */
JNIEXPORT jstring JNICALL
Java_com_couchbase_litecore_fleece_FLValue_toString(JNIEnv *env, jclass clazz, jlong jvalue) {
    FLStringResult str = FLValue_ToString((FLValue) jvalue);
    jstring res = toJString(env, {str.buf, str.size});
    FLSliceResult_Free(str);
    return res;
}

/*
 * Class:     com_couchbase_litecore_fleece_FLValue
 * Method:    toJSON
 * Signature: (J)Ljava/lang/String;
 */
JNIEXPORT jstring JNICALL
Java_com_couchbase_litecore_fleece_FLValue_toJSON(JNIEnv *env, jclass clazz, jlong jvalue) {
    FLStringResult str = FLValue_ToJSON((FLValue) jvalue);
    jstring res = toJString(env, {str.buf, str.size});
    FLSliceResult_Free(str);
    return res;
}

/*
 * Class:     com_couchbase_litecore_fleece_FLValue
 * Method:    toJSON5
 * Signature: (J)Ljava/lang/String;
 */
JNIEXPORT jstring JNICALL
Java_com_couchbase_litecore_fleece_FLValue_toJSON5(JNIEnv *env, jclass clazz, jlong jvalue) {
    FLStringResult str = FLValue_ToJSON5((FLValue) jvalue);
    jstring res = toJString(env, {str.buf, str.size});
    FLSliceResult_Free(str);
    return res;
}
// ----------------------------------------------------------------------------
// FLSliceResult
// ----------------------------------------------------------------------------

/*
 * Class:     com_couchbase_litecore_fleece_FLSliceResult
 * Method:    free
 * Signature: (J)V
 */
JNIEXPORT void JNICALL
Java_com_couchbase_litecore_fleece_FLSliceResult_free(JNIEnv *env, jclass clazz, jlong jslice) {
    FLSliceResult_Free(*(FLSliceResult *) jslice);
}

/*
 * Class:     com_couchbase_litecore_fleece_FLSliceResult
 * Method:    getBuf
 * Signature: (J)[B
 */
JNIEXPORT jbyteArray JNICALL
Java_com_couchbase_litecore_fleece_FLSliceResult_getBuf(JNIEnv *env, jclass clazz, jlong jslice) {
    FLSliceResult *res = (FLSliceResult *) jslice;
    return toJByteArray(env, {res->buf, res->size});
}

/*
 * Class:     com_couchbase_litecore_fleece_FLSliceResult
 * Method:    getSize
 * Signature: (J)J
 */
JNIEXPORT jlong JNICALL
Java_com_couchbase_litecore_fleece_FLSliceResult_getSize(JNIEnv *env, jclass clazz, jlong jslice) {
    return (jlong) ((FLSliceResult *) jslice)->size;
}

