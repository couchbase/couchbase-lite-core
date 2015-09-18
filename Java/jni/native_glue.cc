//
//  native_glue.cpp
//  CBForest
//
//  Created by Jens Alfke on 9/11/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//

#include "native_glue.hh"
#include <assert.h>

using namespace forestdb::jni;


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
        return JNI_VERSION_1_2;
    } else {
        return JNI_ERR;
    }
}


namespace forestdb {
    namespace jni {

        jstringSlice::jstringSlice(JNIEnv *env, jstring js)
        :_env(env),
         _jstr(js)
        {
            jboolean isCopy;
            _cstr = env->GetStringUTFChars(js, &isCopy);
            _slice = slice(_cstr);
        }

        jstringSlice::~jstringSlice() {
            if (_cstr)
                _env->ReleaseStringUTFChars(_jstr, _cstr);
        }


        jbyteArraySlice::jbyteArraySlice(JNIEnv *env, jbyteArray jbytes, bool critical)
        :_env(env),
         _jbytes(jbytes),
         _critical(critical)
        {
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
            jclass xclass = env->FindClass("com/couchbase/cbforest/ForestException");
            assert(xclass);
            jmethodID m = env->GetMethodID(xclass, "throwError", "(II)");
            assert(m);
            env->CallStaticVoidMethod(xclass, m, (jint)error.domain, (jint)error.code);
        }


        jstring toJString(JNIEnv *env, C4Slice s) {
            if (s.buf == NULL)
                return NULL;
            char utf8Buf[s.size + 1];   // FIX: Use heap if string is too long for stack
            ::memcpy(utf8Buf, s.buf, s.size);
            utf8Buf[s.size] = '\0';
            return env->NewStringUTF(utf8Buf);
        }


        jbyteArray toJByteArray(JNIEnv *env, C4Slice s) {
            if (s.buf == NULL)
                return NULL;
            jbyteArray array = env->NewByteArray((jsize)s.size);
            if (array)
                env->SetByteArrayRegion(array, 0, (jsize)s.size, (const jbyte*)s.buf);
            return array;
        }

    }
}
