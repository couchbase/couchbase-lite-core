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
#include <c4.h>
#include <Fleece.h>
#include "com_couchbase_litecore_fleece_FLArray.h"
#include "com_couchbase_litecore_fleece_FLArrayIterator.h"
#include "com_couchbase_litecore_fleece_FLDict.h"
#include "com_couchbase_litecore_fleece_FLDictIterator.h"
#include "com_couchbase_litecore_fleece_FLValue.h"
#include "com_couchbase_litecore_fleece_FLEncoder.h"
#include "com_couchbase_litecore_fleece_FLSliceResult.h"
#include "native_glue.hh"

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
 * Method:    get
 * Signature: (J[B)J
 */
JNIEXPORT jlong JNICALL
Java_com_couchbase_litecore_fleece_FLDict_get(JNIEnv *env, jclass clazz, jlong jdict,
                                              jbyteArray jkeystring) {
    jbyteArraySlice key(env, jkeystring, true);
    return (jlong) FLDict_Get((FLDict) jdict, {((slice) key).buf, ((slice) key).size});
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
    FLError error = {};
    FLString str = FLSharedKey_GetKeyString((FLSharedKeys) jsharedKey, (int) jkeyCode, &error);
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
 * Signature: (J)I
 */
JNIEXPORT jint JNICALL
Java_com_couchbase_litecore_fleece_FLValue_asInt(JNIEnv *env, jclass clazz, jlong jvalue) {
    return (jint) FLValue_AsInt((FLValue) jvalue);
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
    return 0;
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
    FLError error = {};
    FLStringResult json = FLJSON5_ToJSON({((slice) json5).buf, ((slice) json5).size}, &error);
    jstring res = toJString(env, {json.buf, json.size});
    FLSliceResult_Free(json);
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
    ::free((FLSliceResult *) jslice);
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

// ----------------------------------------------------------------------------
// FLValue
// ----------------------------------------------------------------------------

/*
 * Class:     com_couchbase_litecore_fleece_FLEncoder
 * Method:    init
 * Signature: ()J
 */
JNIEXPORT jlong JNICALL
Java_com_couchbase_litecore_fleece_FLEncoder_init(JNIEnv *env,
                                                  jclass clazz) {
    return (jlong) FLEncoder_New();
}

/*
 * Class:     com_couchbase_litecore_fleece_FLEncoder
 * Method:    free
 * Signature: (J)V
 */
JNIEXPORT void JNICALL
Java_com_couchbase_litecore_fleece_FLEncoder_free(JNIEnv *env,
                                                  jclass clazz,
                                                  jlong jencoder) {
    FLEncoder_Free((FLEncoder) jencoder);
}

/*
 * Class:     com_couchbase_litecore_fleece_FLEncoder
 * Method:    setSharedKeys
 * Signature: (JJ)V
 */
JNIEXPORT void JNICALL
Java_com_couchbase_litecore_fleece_FLEncoder_setSharedKeys(JNIEnv *env,
                                                           jclass clazz,
                                                           jlong jencoder,
                                                           jlong jsharedKeys) {
    FLEncoder_SetSharedKeys((FLEncoder) jencoder, (FLSharedKeys) jsharedKeys);
}

/*
 * Class:     com_couchbase_litecore_fleece_FLEncoder
 * Method:    writeNull
 * Signature: (J)Z
 */
JNIEXPORT jboolean JNICALL
Java_com_couchbase_litecore_fleece_FLEncoder_writeNull(JNIEnv *env, jclass clazz, jlong jencoder) {
    return (jboolean) FLEncoder_WriteNull((FLEncoder) jencoder);
}

/*
 * Class:     com_couchbase_litecore_fleece_FLEncoder
 * Method:    writeBool
 * Signature: (JZ)Z
 */
JNIEXPORT jboolean JNICALL
Java_com_couchbase_litecore_fleece_FLEncoder_writeBool(JNIEnv *env, jclass clazz, jlong jencoder,
                                                       jboolean jvalue) {
    return (jboolean) FLEncoder_WriteBool((FLEncoder) jencoder, (bool) jvalue);
}

/*
 * Class:     com_couchbase_litecore_fleece_FLEncoder
 * Method:    writeInt
 * Signature: (JJ)Z
 */
JNIEXPORT jboolean JNICALL
Java_com_couchbase_litecore_fleece_FLEncoder_writeInt(JNIEnv *env, jclass clazz, jlong jencoder,
                                                      jlong jvalue) {
    return (jboolean) FLEncoder_WriteInt((FLEncoder) jencoder, (int64_t) jvalue);
}

/*
 * Class:     com_couchbase_litecore_fleece_FLEncoder
 * Method:    writeFloat
 * Signature: (JF)Z
 */
JNIEXPORT jboolean JNICALL
Java_com_couchbase_litecore_fleece_FLEncoder_writeFloat(JNIEnv *env, jclass clazz, jlong jencoder,
                                                        jfloat jvalue) {
    return (jboolean) FLEncoder_WriteFloat((FLEncoder) jencoder, (float) jvalue);
}

/*
 * Class:     com_couchbase_litecore_fleece_FLEncoder
 * Method:    writeDouble
 * Signature: (JD)Z
 */
JNIEXPORT jboolean JNICALL
Java_com_couchbase_litecore_fleece_FLEncoder_writeDouble(JNIEnv *env, jclass clazz, jlong jencoder,
                                                         jdouble jvalue) {
    return (jboolean) FLEncoder_WriteDouble((FLEncoder) jencoder, (double) jvalue);
}

/*
 * Class:     com_couchbase_litecore_fleece_FLEncoder
 * Method:    writeString
 * Signature: (JLjava/lang/String;)Z
 */
JNIEXPORT jboolean JNICALL
Java_com_couchbase_litecore_fleece_FLEncoder_writeString(JNIEnv *env, jclass clazz, jlong jencoder,
                                                         jstring jvalue) {
    jstringSlice value(env, jvalue);
    return (jboolean) FLEncoder_WriteString((FLEncoder) jencoder,
                                            {((slice) value).buf, ((slice) value).size});
}

/*
 * Class:     com_couchbase_litecore_fleece_FLEncoder
 * Method:    writeData
 * Signature: (J[B)Z
 */
JNIEXPORT jboolean JNICALL
Java_com_couchbase_litecore_fleece_FLEncoder_writeData(JNIEnv *env, jclass clazz, jlong jencoder,
                                                       jbyteArray jvalue) {
    jbyteArraySlice value(env, jvalue, true);
    return (jboolean) FLEncoder_WriteData((FLEncoder) jencoder,
                                          {((slice) value).buf, ((slice) value).size});
}

/*
 * Class:     com_couchbase_litecore_fleece_FLEncoder
 * Method:    beginArray
 * Signature: (JJ)Z
 */
JNIEXPORT jboolean JNICALL
Java_com_couchbase_litecore_fleece_FLEncoder_beginArray(JNIEnv *env, jclass clazz, jlong jencoder,
                                                        jlong jreserve) {
    return (jboolean) FLEncoder_BeginArray((FLEncoder) jencoder, (size_t) jreserve);
}

/*
 * Class:     com_couchbase_litecore_fleece_FLEncoder
 * Method:    endArray
 * Signature: (J)Z
 */
JNIEXPORT jboolean JNICALL
Java_com_couchbase_litecore_fleece_FLEncoder_endArray(JNIEnv *env, jclass clazz, jlong jencoder) {
    return (jboolean) FLEncoder_EndArray((FLEncoder) jencoder);
}

/*
 * Class:     com_couchbase_litecore_fleece_FLEncoder
 * Method:    beginDict
 * Signature: (JJ)Z
 */
JNIEXPORT jboolean JNICALL
Java_com_couchbase_litecore_fleece_FLEncoder_beginDict(JNIEnv *env, jclass clazz, jlong jencoder,
                                                       jlong jreserve) {
    return (jboolean) FLEncoder_BeginDict((FLEncoder) jencoder, (size_t) jreserve);
}

/*
 * Class:     com_couchbase_litecore_fleece_FLEncoder
 * Method:    endDict
 * Signature: (J)Z
 */
JNIEXPORT jboolean JNICALL
Java_com_couchbase_litecore_fleece_FLEncoder_endDict(JNIEnv *env, jclass clazz, jlong jencoder) {
    return (jboolean) FLEncoder_EndDict((FLEncoder) jencoder);
}

/*
 * Class:     com_couchbase_litecore_fleece_FLEncoder
 * Method:    writeKey
 * Signature: (J[B)Z
 */
JNIEXPORT jboolean JNICALL
Java_com_couchbase_litecore_fleece_FLEncoder_writeKey(JNIEnv *env, jclass clazz, jlong jencoder,
                                                      jstring jkey) {
    if (jkey == NULL) return false;
    jstringSlice key(env, jkey);
    return (jboolean) FLEncoder_WriteKey((FLEncoder) jencoder,
                                         {((slice) key).buf, ((slice) key).size});
}

/*
 * Class:     com_couchbase_litecore_fleece_FLEncoder
 * Method:    writeValue
 * Signature: (JJ)Z
 */
JNIEXPORT jboolean JNICALL
Java_com_couchbase_litecore_fleece_FLEncoder_writeValue(JNIEnv *env, jclass clazz, jlong jencoder,
                                                        jlong jvalue) {
    return (jboolean) FLEncoder_WriteValue((FLEncoder) jencoder, (FLValue) jvalue);
}

/*
 * Class:     com_couchbase_litecore_fleece_FLEncoder
 * Method:    finish
 * Signature: (J)[B
 */
JNIEXPORT jbyteArray JNICALL
Java_com_couchbase_litecore_fleece_FLEncoder_finish(JNIEnv *env, jclass clazz, jlong jencoder) {
    FLError error;
    FLSliceResult result = FLEncoder_Finish((FLEncoder) jencoder, &error);
    jbyteArray res = toJByteArray(env, {result.buf, result.size});
    FLSliceResult_Free(result);
    return res;
}
