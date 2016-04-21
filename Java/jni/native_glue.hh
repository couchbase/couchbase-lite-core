//
//  native_glue.hh
//  CBForest
//
//  Created by Jens Alfke on 9/11/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//

#ifndef native_glue_hpp
#define native_glue_hpp

#include <jni.h>
#include "c4Database.h"
#include "slice.hh"
#include <vector>

namespace cbforest {
    namespace jni {

// Soft limit of number of local JNI refs to use. Even using PushLocalFrame(), you may not get as
// many refs as you asked for. At least, that's what happens on Android: the new frame won't have
// more than 512 refs available. So 200 is being conservative.
static const jsize MaxLocalRefsToUse = 200;

extern JavaVM *gJVM;

bool initDatabase(JNIEnv*);     // Implemented in native_database.cc
bool initDocument(JNIEnv*);     // Implemented in native_document.cc
bool initQueryIterator(JNIEnv*);// Implemented in native_queryIterator.cc
bool initView(JNIEnv*);         // Implemented in native_view.cc

// Creates a temporary slice value from a Java String object
class jstringSlice {
public:
    jstringSlice(JNIEnv *env, jstring js);
    ~jstringSlice();

    jstringSlice(jstringSlice&& s) // move constructor
    :_slice(s._slice), _env(s._env), _jstr(s._jstr)
    { s._env = NULL; s._slice.buf = NULL; }

    operator slice()    {return _slice;}
    operator C4Slice()  {return {_slice.buf, _slice.size};}

    // Copies the string data and releases the JNI local ref.
    void copyAndReleaseRef();

private:
    slice _slice;
    JNIEnv *_env;
    jstring _jstr;
};


// Creates a temporary slice value from a Java byte[], attempting to avoid copying
class jbyteArraySlice {
public:
    // Warning: If `critical` is true, you cannot make any further JNI calls (except other
    // critical accesses) until this object goes out of scope or is deleted.
    jbyteArraySlice(JNIEnv *env, jbyteArray jbytes, bool critical =false);
    ~jbyteArraySlice();

    jbyteArraySlice(jbyteArraySlice&& s) // move constructor
    :_slice(s._slice), _env(s._env), _jbytes(s._jbytes), _critical(s._critical)
    { s._slice = slice::null; }

    operator slice()    {return _slice;}
    operator C4Slice()  {return {_slice.buf, _slice.size};}

    // Copies a Java byte[] to an alloc_slice
    static alloc_slice copy(JNIEnv *env, jbyteArray jbytes);

private:
    slice _slice;
    JNIEnv *_env;
    jbyteArray _jbytes;
    bool _critical;
};

// Creates a Java String from the contents of a C4Slice.
jstring toJString(JNIEnv*, C4Slice);

// Creates a Java byte[] from the contents of a C4Slice.
jbyteArray toJByteArray(JNIEnv*, C4Slice);

// Sets a Java exception based on the CBForest error.
void throwError(JNIEnv*, C4Error);

// Copies an array of handles from a Java long[] to a C++ vector.
template <typename T>
std::vector<T> handlesToVector(JNIEnv *env, jlongArray jhandles) {
    jsize count = env->GetArrayLength(jhandles);
    std::vector<T> objects(count);
    if (count > 0) {
        jboolean isCopy;
        auto handles = env->GetLongArrayElements(jhandles, &isCopy);
        for(jsize i = 0; i < count; i++) {
            objects[i] = (T)handles[i];
        }
        env->ReleaseLongArrayElements(jhandles, handles, JNI_ABORT);
    }
    return objects;
}

// Copies an encryption key to a C4EncryptionKey. Returns false on exception.
bool getEncryptionKey(JNIEnv *env,
                      jint keyAlg,
                      jbyteArray jKeyBytes,
                      C4EncryptionKey *outKey);

    }
}

#endif /* native_glue_hpp */
