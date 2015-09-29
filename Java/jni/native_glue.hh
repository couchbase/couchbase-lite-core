//
//  native_glue.hh
//  CBForest
//
//  Created by Jens Alfke on 9/11/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//

#ifndef native_glue_hpp
#define native_glue_hpp

#include <JavaVM/jni.h>
#include "c4.h"
#include "slice.hh"

namespace forestdb {
    namespace jni {

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

    operator slice()    {return _slice;}
    operator C4Slice()  {return (C4Slice){_slice.buf, _slice.size};}

private:
    slice _slice;
    JNIEnv *_env;
    jstring _jstr;
    const char *_cstr;
};


// Creates a temporary slice value from a Java byte[], attempting to avoid copying
class jbyteArraySlice {
public:
    jbyteArraySlice(JNIEnv *env, jbyteArray jbytes, bool critical =false);
    ~jbyteArraySlice();

    operator slice()    {return _slice;}
    operator C4Slice()  {return (C4Slice){_slice.buf, _slice.size};}

    // Copies a Java byte[] to an alloc_slice
    static alloc_slice copy(JNIEnv *env, jbyteArray jbytes);

private:
    slice _slice;
    JNIEnv *_env;
    jbyteArray _jbytes;
    bool _critical;
};


jstring toJString(JNIEnv*, C4Slice);

jbyteArray toJByteArray(JNIEnv*, C4Slice);

void throwError(JNIEnv*, C4Error);

    }
}

#endif /* native_glue_hpp */
