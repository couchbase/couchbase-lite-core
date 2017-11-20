//
// Created by hideki on 11/9/17.
//

#include "native_mutable.hh"
#include "com_couchbase_litecore_fleece_MValue.h"

using namespace fleece;
using namespace fleeceapi;
using namespace litecore::jni;

// ----------------------------------------------------------------------------
// MValue + JNI implementation (See MValue+ObjC.mm)
// ----------------------------------------------------------------------------
static jclass cls_FleeceArray;              // global reference to FleeceArray
static jmethodID m_FleeceArray_init;        // callback method for FleeceArray(long, long)

static jclass cls_FleeceDict;               // global reference to FleeceDict
static jmethodID m_FleeceDict_init;         // callback method for FleeceDict(long, long)

static jclass    cls_CBLFleece;            // global reference to Dictionary
static jmethodID m_MValue_toDictionary;    // callback method for MValue_toDictionary(long, long)
static jmethodID m_MValue_toArray;         // callback method for MValue_toArray(long, long)


static jclass cls_FLValue;                  // global reference to FLValue
static jmethodID m_FLValue_toObject;           // static method for asObject(long)

static jclass cls_MValue;                   // global reference to MValue
static jmethodID m_MValue_getFLCollection;     // static method for getFLCollection(Object)
static jmethodID m_MValue_encodeNative;        // static method for encodeNative(long, Object)

bool litecore::jni::initMValue(JNIEnv *env) {
    // Find FleeceDict class and constructor
    {
        jclass localClass = env->FindClass("com/couchbase/litecore/fleece/FleeceDict");
        if (!localClass)
            return false;
        cls_FleeceDict = reinterpret_cast<jclass>(env->NewGlobalRef(localClass));
        if (!cls_FleeceDict)
            return false;
        m_FleeceDict_init = env->GetMethodID(cls_FleeceDict, "<init>", "(JJ)V");
        if (!m_FleeceDict_init)
            return false;
    }

    // Find FleeceArray class and constructor
    {
        jclass localClass = env->FindClass("com/couchbase/litecore/fleece/FleeceArray");
        if (!localClass)
            return false;
        cls_FleeceArray = reinterpret_cast<jclass>(env->NewGlobalRef(localClass));
        if (!cls_FleeceArray)
            return false;
        m_FleeceArray_init = env->GetMethodID(cls_FleeceArray, "<init>", "(JJ)V");
        if (!m_FleeceArray_init)
            return false;
    }

    // Find Dictionary class and constructor
    {
        jclass localClass = env->FindClass("com/couchbase/lite/CBLFleece");
        if (!localClass)
            return false;
        cls_CBLFleece = reinterpret_cast<jclass>(env->NewGlobalRef(localClass));
        if (!cls_CBLFleece)
            return false;
        m_MValue_toDictionary = env->GetStaticMethodID(cls_CBLFleece, "MValue_toDictionary", "(JJ)Ljava/lang/Object;");
        if (!m_MValue_toDictionary)
            return false;
        m_MValue_toArray = env->GetStaticMethodID(cls_CBLFleece, "MValue_toArray", "(JJ)Ljava/lang/Object;");
        if (!m_MValue_toArray)
            return false;
    }


    // Find FLValue class and asObject(long) static method
    {
        jclass localClass = env->FindClass("com/couchbase/litecore/fleece/FLValue");
        if (!localClass)
            return false;
        cls_FLValue = reinterpret_cast<jclass>(env->NewGlobalRef(localClass));
        if (!cls_FLValue)
            return false;
        m_FLValue_toObject = env->GetStaticMethodID(cls_FLValue, "toObject",
                                                    "(J)Ljava/lang/Object;");
        if (!m_FLValue_toObject)
            return false;
    }

    // Find MValue class and getFLCollection(Object) static method
    {
        jclass localClass = env->FindClass("com/couchbase/litecore/fleece/MValue");
        if (!localClass)
            return false;
        cls_MValue = reinterpret_cast<jclass>(env->NewGlobalRef(localClass));
        if (!cls_MValue)
            return false;
        m_MValue_getFLCollection = env->GetStaticMethodID(cls_MValue, "getFLCollection",
                                                          "(Ljava/lang/Object;)J");
        if (!m_MValue_getFLCollection)
            return false;
        m_MValue_encodeNative = env->GetStaticMethodID(cls_MValue, "encodeNative",
                                                       "(JLjava/lang/Object;)V");
        if (!m_MValue_encodeNative)
            return false;
    }
    return true;
}

static JNIRef *createFleeceArray(jlong hMv, jlong hParent) {
    JNIRef *jniRef = NULL;
    JNIEnv *env = NULL;
    jint getEnvStat = gJVM->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (getEnvStat == JNI_OK) {
        jobject newObj = env->NewObject(cls_FleeceArray, m_FleeceArray_init, hMv, hParent);
        jniRef = new JNIRef(env, newObj);
    } else if (getEnvStat == JNI_EDETACHED) {
        if (gJVM->AttachCurrentThread(&env, NULL) == 0) {
            jobject newObj = env->NewObject(cls_FleeceArray, m_FleeceArray_init, hMv, hParent);
            jniRef = new JNIRef(env, newObj);
            if (gJVM->DetachCurrentThread() != 0) {
            }
        }
    }
    return jniRef;
}

static JNIRef *createFleeceDict(jlong hMv, jlong hParent) {
    JNIRef *jniRef = NULL;
    JNIEnv *env = NULL;
    jint getEnvStat = gJVM->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (getEnvStat == JNI_OK) {
        jobject newObj = env->NewObject(cls_FleeceDict, m_FleeceDict_init, hMv, hParent);
        jniRef = new JNIRef(env, newObj);
    } else if (getEnvStat == JNI_EDETACHED) {
        if (gJVM->AttachCurrentThread(&env, NULL) == 0) {
            jobject newObj = env->NewObject(cls_FleeceDict, m_FleeceDict_init, hMv, hParent);
            jniRef = new JNIRef(env, newObj);
            if (gJVM->DetachCurrentThread() != 0) {
            }
        }
    }
    return jniRef;
}

static JNIRef *createObject(jlong hFLValue) {
    JNIRef *jniRef = NULL;
    JNIEnv *env = NULL;
    jint getEnvStat = gJVM->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (getEnvStat == JNI_OK) {
        jobject newObj = env->CallStaticObjectMethod(cls_FLValue,
                                                     m_FLValue_toObject,
                                                     hFLValue);
        jniRef = new JNIRef(env, newObj);
    } else if (getEnvStat == JNI_EDETACHED) {
        if (gJVM->AttachCurrentThread(&env, NULL) == 0) {
            jobject newObj = env->CallStaticObjectMethod(cls_FLValue,
                                                         m_FLValue_toObject,
                                                         hFLValue);
            jniRef = new JNIRef(env, newObj);
            if (gJVM->DetachCurrentThread() != 0) {
            }
        }
    }
    return jniRef;
}
static JNIRef *createArray(jlong hMv, jlong hParent) {
    JNIRef *jniRef = NULL;
    JNIEnv *env = NULL;
    jint getEnvStat = gJVM->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (getEnvStat == JNI_OK) {
        jobject newObj = env->CallStaticObjectMethod(cls_CBLFleece, m_MValue_toArray, hMv, hParent);
        jniRef = new JNIRef(env, newObj);
    } else if (getEnvStat == JNI_EDETACHED) {
        if (gJVM->AttachCurrentThread(&env, NULL) == 0) {
            jobject newObj = env->CallStaticObjectMethod(cls_CBLFleece, m_MValue_toArray, hMv, hParent);
            jniRef = new JNIRef(env, newObj);
            if (gJVM->DetachCurrentThread() != 0) {
            }
        }
    }
    return jniRef;
}

static JNIRef *createDict(jlong hMv, jlong hParent) {
    JNIRef *jniRef = NULL;
    JNIEnv *env = NULL;
    jint getEnvStat = gJVM->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (getEnvStat == JNI_OK) {
        jobject newObj = env->CallStaticObjectMethod(cls_CBLFleece, m_MValue_toDictionary, hMv, hParent);
        jniRef = new JNIRef(env, newObj);
    } else if (getEnvStat == JNI_EDETACHED) {
        if (gJVM->AttachCurrentThread(&env, NULL) == 0) {
            jobject newObj = env->CallStaticObjectMethod(cls_CBLFleece, m_MValue_toDictionary, hMv, hParent);
            jniRef = new JNIRef(env, newObj);
            if (gJVM->DetachCurrentThread() != 0) {
            }
        }
    }
    return jniRef;
}

// ----------------------------------------------------------------------------
// MValue + JNI implementation (See MValue+ObjC.mm)
// ----------------------------------------------------------------------------
template<>
JNative JMValue::toNative(JMValue *mv, JMCollection *parent, bool &cacheIt) {
    Value value = mv->value();
    switch (value.type()) {
        case kFLArray: {
            cacheIt = true;
            return JNative(createArray((jlong) mv, (jlong) parent));
        }
        case kFLDict: {
            cacheIt = true;
            return JNative(createDict((jlong) mv, (jlong) parent));
        }
        default: {
            cacheIt = true;
            FLValue val = (FLValue) mv->value();
            return JNative(createObject((jlong) val));
        }
    }
}

template<>
JMCollection *JMValue::collectionFromNative(JNative native) {
    jobject obj = native->native();
    JNIEnv *env = NULL;
    jint getEnvStat = gJVM->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (getEnvStat == JNI_OK)
        return (JMCollection *) env->CallStaticLongMethod(cls_MValue, m_MValue_getFLCollection,
                                                          obj);
    return NULL;
}

template<>
void JMValue::encodeNative(Encoder &enc, JNative native) {
    jobject obj = native->native();
    JNIEnv *env = NULL;
    jint getEnvStat = gJVM->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (getEnvStat == JNI_OK) {
        env->CallStaticVoidMethod(cls_MValue, m_MValue_encodeNative, (jlong) &enc, obj);
    }
}

// ----------------------------------------------------------------------------
// MValue JNI bindings
// ----------------------------------------------------------------------------

/*
 * Class:     com_couchbase_litecore_fleece_MValue
 * Method:    free
 * Signature: (J)V
 */
JNIEXPORT void JNICALL
Java_com_couchbase_litecore_fleece_MValue_free(JNIEnv *env, jclass clazz, jlong jmval) {
    delete (JMValue *) jmval;
}

/*
 * Class:     com_couchbase_litecore_fleece_MValue
 * Method:    init
 * Signature: (Ljava/lang/Object;)J
 */
JNIEXPORT jlong JNICALL
Java_com_couchbase_litecore_fleece_MValue_init(JNIEnv *env, jclass clazz, jobject jnative) {
    return (jlong) new JMValue(new JNIRef(env, jnative));
}

/*
 * Class:     com_couchbase_litecore_fleece_MValue
 * Method:    value
 * Signature: (J)J
 */
JNIEXPORT jlong JNICALL
Java_com_couchbase_litecore_fleece_MValue_value(JNIEnv *env, jclass clazz, jlong jmval) {
    return (jlong) (FLValue) ((JMValue *) jmval)->value();
}

/*
 * Class:     com_couchbase_litecore_fleece_MValue
 * Method:    isEmpty
 * Signature: (J)Z
 */
JNIEXPORT jboolean JNICALL
Java_com_couchbase_litecore_fleece_MValue_isEmpty(JNIEnv *env, jclass clazz, jlong jmval) {
    return ((JMValue *) jmval)->isEmpty();
}

/*
 * Class:     com_couchbase_litecore_fleece_MValue
 * Method:    isMutated
 * Signature: (J)Z
 */
JNIEXPORT jboolean JNICALL
Java_com_couchbase_litecore_fleece_MValue_isMutated(JNIEnv *env, jclass clazz, jlong jmval) {
    return ((JMValue *) jmval)->isMutated();
}

/*
 * Class:     com_couchbase_litecore_fleece_MValue
 * Method:    hasNative
 * Signature: (J)Z
 */
JNIEXPORT jboolean JNICALL
Java_com_couchbase_litecore_fleece_MValue_hasNative(JNIEnv *env, jclass clazz, jlong jmval) {
    return (jboolean) ((JMValue *) jmval)->hasNative();
}

/*
 * Class:     com_couchbase_litecore_fleece_MValue
 * Method:    asNative
 * Signature: (JJ)Ljava/lang/Object;
 */
JNIEXPORT jobject JNICALL
Java_com_couchbase_litecore_fleece_MValue_asNative(JNIEnv *env, jclass clazz, jlong jmval,
                                                   jlong jparent) {
    JNative ref = ((JMValue *) jmval)->asNative((JMCollection *) jparent);
    return ref->native();
}

/*
 * Class:     com_couchbase_litecore_fleece_MValue
 * Method:    encodeTo
 * Signature: (JJ)V
 */
JNIEXPORT void JNICALL
Java_com_couchbase_litecore_fleece_MValue_encodeTo(JNIEnv *env, jclass clazz, jlong jmval,
                                                   jlong jenc) {
    Encoder *enc = (Encoder *) jenc;
    ((JMValue *) jmval)->encodeTo(*enc);
}

/*
 * Class:     com_couchbase_litecore_fleece_MValue
 * Method:    mutate
 * Signature: (J)V
 */
JNIEXPORT void JNICALL
Java_com_couchbase_litecore_fleece_MValue_mutate(JNIEnv *env, jclass clazz, jlong jmval) {
    ((JMValue *) jmval)->mutate();
}
