//
// native_c4.cc
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
#include "com_couchbase_litecore_C4.h"
#include "com_couchbase_litecore_C4Log.h"
#include "com_couchbase_litecore_C4Key.h"
#include "mbedtls/pkcs5.h"
#include "native_glue.hh"

using namespace litecore;
using namespace litecore::jni;

// ----------------------------------------------------------------------------
// com_couchbase_litecore_C4
// ----------------------------------------------------------------------------
/*
 * Class:     com_couchbase_litecore_C4
 * Method:    setenv
 * Signature: (Ljava/lang/String;Ljava/lang/String;I)V
 */
JNIEXPORT void JNICALL
Java_com_couchbase_litecore_C4_setenv(JNIEnv *env, jclass clazz, jstring jname, jstring jvalue,
                                      jint overwrite) {
    jstringSlice name(env, jname);
    jstringSlice value(env, jvalue);
    setenv(((slice) name).cString(), ((slice) value).cString(), overwrite);
}

/*
 * Class:     com_couchbase_litecore_C4
 * Method:    getenv
 * Signature: (Ljava/lang/String;)Ljava/lang/String;
 */
JNIEXPORT jstring JNICALL
Java_com_couchbase_litecore_C4_getenv(JNIEnv *env, jclass clazz, jstring jname) {
    jstringSlice name(env, jname);
    return env->NewStringUTF(getenv(((slice) name).cString()));
}

/*
 * Class:     com_couchbase_litecore_C4
 * Method:    getBuildInfo
 * Signature: ()Ljava/lang/String;
 */
JNIEXPORT jstring JNICALL Java_com_couchbase_litecore_C4_getBuildInfo(JNIEnv *env, jclass clazz) {
    C4StringResult result = c4_getBuildInfo();
    jstring jstr = toJString(env, result);
    c4slice_free(result);
    return jstr;
}

/*
 * Class:     com_couchbase_litecore_C4
 * Method:    getVersion
 * Signature: ()Ljava/lang/String;
 */
JNIEXPORT jstring JNICALL Java_com_couchbase_litecore_C4_getVersion(JNIEnv *env, jclass clazz) {
    C4StringResult result = c4_getVersion();
    jstring jstr = toJString(env, result);
    c4slice_free(result);
    return jstr;
}

// ----------------------------------------------------------------------------
// com_couchbase_litecore_C4Log
// ----------------------------------------------------------------------------

/*
 * Class:     com_couchbase_litecore_C4Log
 * Method:    setLevel
 * Signature: (Ljava/lang/String;I)V
 */
JNIEXPORT void JNICALL
Java_com_couchbase_litecore_C4Log_setLevel(JNIEnv *env, jclass clazz, jstring jdomain,
                                           jint jlevel) {
    jstringSlice domain(env, jdomain);
    C4LogDomain logDomain = c4log_getDomain(((slice) domain).cString(), false);
    if (logDomain)
        c4log_setLevel(logDomain, (C4LogLevel) jlevel);
}

// ----------------------------------------------------------------------------
// com_couchbase_litecore_C4Key
// ----------------------------------------------------------------------------

/*
 * Class:     com_couchbase_litecore_C4Key
 * Method:    pbkdf2
 * Signature: (Ljava/lang/String;[BII)[B
 */
JNIEXPORT jbyteArray JNICALL Java_com_couchbase_litecore_C4Key_pbkdf2
        (JNIEnv *env, jclass clazz, jstring jpassword, jbyteArray jsalt, jint jiteration,
         jint jkeyLen) {

    // PBKDF2 (Password-Based Key Derivation Function 2)
    // https://en.wikipedia.org/wiki/PBKDF2
    // https://www.ietf.org/rfc/rfc2898.txt
    //
    // algorithm: PBKDF2
    // hash: SHA1
    // iteration: ? (64000)
    // key length: ? (16)

    if (jpassword == NULL || jsalt == NULL)
        return NULL;

    // Password:
    const char *password = env->GetStringUTFChars(jpassword, NULL);
    int passwordSize = (int) env->GetStringLength(jpassword);

    // Salt:
    int saltSize = env->GetArrayLength(jsalt);
    unsigned char *salt = new unsigned char[saltSize];
    env->GetByteArrayRegion(jsalt, 0, saltSize, reinterpret_cast<jbyte *>(salt));

    // Rounds
    const int iteration = (const int) jiteration;

    // PKCS5 PBKDF2 HMAC SHA256
    const int keyLen = (const int) jkeyLen;
    unsigned char key[keyLen];

    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);

    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
    if (info == NULL) {
        // error
        mbedtls_md_free(&ctx);
        env->ReleaseStringUTFChars(jpassword, password);
        delete[] salt;
        return NULL;
    }

    int status = 0;
    if ((status = mbedtls_md_setup(&ctx, info, 1)) == 0)
        status = mbedtls_pkcs5_pbkdf2_hmac(&ctx, (const unsigned char *) password, passwordSize,
                                           salt, saltSize, iteration, keyLen, key);

    mbedtls_md_free(&ctx);

    // Release memory:
    env->ReleaseStringUTFChars(jpassword, password);
    delete[] salt;

    // Return null if not success:
    if (status != 0)
        return NULL;

    // Return result:
    jbyteArray result = env->NewByteArray(keyLen);
    env->SetByteArrayRegion(result, 0, keyLen, (jbyte *) key);
    return result;
}