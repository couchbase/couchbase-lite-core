//
//  native_glue.cpp
//  CBForest
//
//  Created by Jens Alfke on 9/11/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#include "native_glue.hh"
#include "c4Database.h"
#include "fdb_errors.h"
#include <assert.h>

using namespace cbforest::jni;


// Will be called by JNI when the library is loaded
JNIEXPORT jint JNICALL
JNI_OnLoad(JavaVM *jvm, void *reserved)
{
    JNIEnv *env;
    if (jvm->GetEnv((void **)&env, JNI_VERSION_1_2) == JNI_OK
            && initDatabase(env)
            && initDocument(env)
            && initQueryIterator(env)
            && initView(env)) {
        assert(gJVM == NULL);
        gJVM = jvm;
        return JNI_VERSION_1_2;
    } else {
        return JNI_ERR;
    }
}


namespace cbforest {
    namespace jni {

        JavaVM *gJVM;
        
        jstringSlice::jstringSlice(JNIEnv *env, jstring js)
        :_env(NULL)
        {
            assert(env != NULL);
            if (js != NULL) {
                jboolean isCopy;
                const char *cstr = env->GetStringUTFChars(js, &isCopy);
                if (!cstr)
                    return; // Would it be better to throw an exception?
                _slice = slice(cstr);
                _jstr = js;
                _env = env;
            }
        }

        jstringSlice::~jstringSlice() {
            if (_env)
                _env->ReleaseStringUTFChars(_jstr, (const char*)_slice.buf);
            else if (_slice.buf)
                free((void*)_slice.buf);        // detached
        }

        void jstringSlice::copyAndReleaseRef() {
            if (_env) {
                auto cstr = (const char*)_slice.buf;
                _slice = _slice.copy();
                _env->ReleaseStringUTFChars(_jstr, cstr);
                _env->DeleteLocalRef(_jstr);
                _env = NULL;
            }
        }


        // ATTN: In critical, should not call any other JNI methods.
        // http://docs.oracle.com/javase/6/docs/technotes/guides/jni/spec/functions.html
        jbyteArraySlice::jbyteArraySlice(JNIEnv *env, jbyteArray jbytes, bool critical)
        :_env(env),
         _jbytes(jbytes),
         _critical(critical)
        {
            if(jbytes == NULL){
                _slice.buf = NULL;
                _slice.size = 0;
                return;
            }

            jboolean isCopy;
            if (critical)
                _slice.buf = env->GetPrimitiveArrayCritical(jbytes, &isCopy);
            else
                _slice.buf = env->GetByteArrayElements(jbytes, &isCopy);
            _slice.size = env->GetArrayLength(jbytes);
        }

        jbyteArraySlice::~jbyteArraySlice() {
            if (_slice.buf) {
                if (_critical)
                    _env->ReleasePrimitiveArrayCritical(_jbytes, (void*)_slice.buf, JNI_ABORT);
                else
                    _env->ReleaseByteArrayElements(_jbytes, (jbyte*)_slice.buf, JNI_ABORT);
            }
        }

        alloc_slice jbyteArraySlice::copy(JNIEnv *env, jbyteArray jbytes) {
            jsize size = env->GetArrayLength(jbytes);
            alloc_slice slice(size);
            env->GetByteArrayRegion(jbytes, 0, size, (jbyte*)slice.buf);
            return slice;
        }


        void throwError(JNIEnv *env, C4Error error) {
            if (env->ExceptionOccurred())
                return;
            jclass xclass = env->FindClass("com/couchbase/cbforest/ForestException");
            assert(xclass); // if we can't even throw an exception, we're really fuxored
            jmethodID m = env->GetStaticMethodID(xclass, "throwException",
                                                 "(IILjava/lang/String;)V");
            assert(m);

            C4SliceResult msgSlice = c4error_getMessage(error);
            jstring msg = toJString(env, msgSlice);
            c4slice_free(msgSlice);

            env->CallStaticVoidMethod(xclass, m, (jint)error.domain, (jint)error.code, msg);
        }


        jstring toJString(JNIEnv *env, C4Slice s) {
            if (s.buf == NULL)
                return NULL;
            std::string utf8Buf((char*)s.buf, s.size);
            // NOTE: This return value will be taken care by JVM. So not necessary to free by our self
            return env->NewStringUTF(utf8Buf.c_str());
        }

        jbyteArray toJByteArray(JNIEnv *env, C4Slice s) {
            if (s.buf == NULL)
                return NULL;
            // NOTE: Local reference is taken care by JVM.
            // http://docs.oracle.com/javase/6/docs/technotes/guides/jni/spec/functions.html#global_local
            jbyteArray array = env->NewByteArray((jsize)s.size);
            if (array)
                env->SetByteArrayRegion(array, 0, (jsize)s.size, (const jbyte*)s.buf);
            return array;
        }

        bool getEncryptionKey(JNIEnv *env, jint keyAlg, jbyteArray jKeyBytes,
                              C4EncryptionKey *outKey)
        {
            outKey->algorithm = (C4EncryptionAlgorithm)keyAlg;
            if (keyAlg != kC4EncryptionNone) {
                jbyteArraySlice keyBytes(env, jKeyBytes);
                cbforest::slice keySlice = keyBytes;
                if (!keySlice.buf || keySlice.size > sizeof(outKey->bytes)) {
                    throwError(env, C4Error{ForestDBDomain, FDB_RESULT_CRYPTO_ERROR});
                    return false;
                }
                memset(outKey->bytes, 0, sizeof(outKey->bytes));
                memcpy(outKey->bytes, keySlice.buf, keySlice.size);
            }
            return true;
        }
    }
}
