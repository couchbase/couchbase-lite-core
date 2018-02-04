//
// native_c4replicator.cc
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
#include <c4.h>
#include <c4Replicator.h>
#include <c4Base.h>
#include "com_couchbase_litecore_C4Replicator.h"
#include "native_glue.hh"
#include "logging.h"

using namespace litecore;
using namespace litecore::jni;

// ----------------------------------------------------------------------------
// com_couchbase_litecore_C4Replicator
// ----------------------------------------------------------------------------

// C4Replicator
static jclass cls_C4Replicator;           // global reference
static jmethodID m_C4Replicator_statusChangedCallback; // statusChangedCallback method
static jmethodID m_C4Replicator_documentErrorCallback; // documentErrorCallback method

// C4ReplicatorStatus
static jclass cls_C4ReplStatus; // global reference
static jmethodID m_C4ReplStatus_init;
static jfieldID f_C4ReplStatus_activityLevel;
static jfieldID f_C4ReplStatus_progressUnitsCompleted;
static jfieldID f_C4ReplStatus_progressUnitsTotal;
static jfieldID f_C4ReplStatus_progressDocumentCount;
static jfieldID f_C4ReplStatus_errorDomain;
static jfieldID f_C4ReplStatus_errorCode;
static jfieldID f_C4ReplStatus_errorInternalInfo;

bool litecore::jni::initC4Replicator(JNIEnv *env) {
    // Find `C4Replicator` class and `statusChangedCallback(long, C4ReplicatorStatus )` static method for callback
    {
        jclass localClass = env->FindClass("com/couchbase/litecore/C4Replicator");
        if (!localClass)
            return false;

        cls_C4Replicator = reinterpret_cast<jclass>(env->NewGlobalRef(localClass));
        if (!cls_C4Replicator)
            return false;

        m_C4Replicator_statusChangedCallback = env->GetStaticMethodID(cls_C4Replicator,
                                                                      "statusChangedCallback",
                                                                      "(JLcom/couchbase/litecore/C4ReplicatorStatus;)V");
        if (!m_C4Replicator_statusChangedCallback)
            return false;

        m_C4Replicator_documentErrorCallback = env->GetStaticMethodID(cls_C4Replicator,
                                                                      "documentErrorCallback",
                                                                      "(JZLjava/lang/String;IIIZ)V");
        if (!m_C4Replicator_documentErrorCallback)
            return false;
    }

    // C4ReplicatorStatus, constructor, and fields
    {
        jclass localClass = env->FindClass("com/couchbase/litecore/C4ReplicatorStatus");
        if (!localClass)
            return false;

        cls_C4ReplStatus = reinterpret_cast<jclass>(env->NewGlobalRef(localClass));
        if (!cls_C4ReplStatus)
            return false;

        m_C4ReplStatus_init = env->GetMethodID(cls_C4ReplStatus, "<init>", "()V");
        if (!m_C4ReplStatus_init)
            return false;

        f_C4ReplStatus_activityLevel = env->GetFieldID(cls_C4ReplStatus, "activityLevel", "I");
        if (!f_C4ReplStatus_activityLevel)
            return false;

        f_C4ReplStatus_progressUnitsCompleted = env->GetFieldID(cls_C4ReplStatus,
                                                                "progressUnitsCompleted", "J");
        if (!f_C4ReplStatus_progressUnitsCompleted)
            return false;

        f_C4ReplStatus_progressUnitsTotal = env->GetFieldID(cls_C4ReplStatus, "progressUnitsTotal",
                                                            "J");
        if (!f_C4ReplStatus_progressUnitsTotal)
            return false;

        f_C4ReplStatus_progressDocumentCount = env->GetFieldID(cls_C4ReplStatus,
                                                               "progressDocumentCount", "J");
        if (!f_C4ReplStatus_progressUnitsTotal)
            return false;

        f_C4ReplStatus_errorDomain = env->GetFieldID(cls_C4ReplStatus, "errorDomain", "I");
        if (!f_C4ReplStatus_errorDomain)
            return false;

        f_C4ReplStatus_errorCode = env->GetFieldID(cls_C4ReplStatus, "errorCode", "I");
        if (!f_C4ReplStatus_errorCode)
            return false;

        f_C4ReplStatus_errorInternalInfo = env->GetFieldID(cls_C4ReplStatus, "errorInternalInfo",
                                                           "I");
        if (!f_C4ReplStatus_errorInternalInfo)
            return false;
    }
    return true;
}

static jobject toJavaObject(JNIEnv *env, C4ReplicatorStatus status) {
    jobject obj = env->NewObject(cls_C4ReplStatus, m_C4ReplStatus_init);
    env->SetIntField(obj, f_C4ReplStatus_activityLevel, (int) status.level);
    env->SetLongField(obj, f_C4ReplStatus_progressUnitsCompleted,
                      (long) status.progress.unitsCompleted);
    env->SetLongField(obj, f_C4ReplStatus_progressUnitsTotal, (long) status.progress.unitsTotal);
    env->SetLongField(obj, f_C4ReplStatus_progressDocumentCount,
                      (long) status.progress.documentCount);
    env->SetIntField(obj, f_C4ReplStatus_errorDomain, (int) status.error.domain);
    env->SetIntField(obj, f_C4ReplStatus_errorCode, (int) status.error.code);
    env->SetIntField(obj, f_C4ReplStatus_errorInternalInfo, (int) status.error.internal_info);
    return obj;
}

/**
 * Callback a client can register, to get progress information.
 * This will be called on arbitrary background threads, and should not block.
 *
 * @param repl
 * @param status
 * @param ctx
 */
static void statusChangedCallback(C4Replicator *repl, C4ReplicatorStatus status, void *ctx) {

    LOGI("[NATIVE] C4Replicator.statusChangedCallback() repl -> 0x%p status -> %d", repl,
         status.level);

    JNIEnv *env = NULL;
    jint getEnvStat = gJVM->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
    if (getEnvStat == JNI_OK) {
        env->CallStaticVoidMethod(cls_C4Replicator, m_C4Replicator_statusChangedCallback,
                                  (jlong) repl,
                                  toJavaObject(env, status));
    } else if (getEnvStat == JNI_EDETACHED) {
        if (gJVM->AttachCurrentThread(&env, NULL) == 0) {
            env->CallStaticVoidMethod(cls_C4Replicator, m_C4Replicator_statusChangedCallback,
                                      (jlong) repl,
                                      toJavaObject(env, status));
            if (gJVM->DetachCurrentThread() != 0)
                LOGE("doRequestClose(): Failed to detach the current thread from a Java VM");
        } else {
            LOGE("doRequestClose(): Failed to attaches the current thread to a Java VM");
        }
    } else {
        LOGE("doClose(): Failed to get the environment: getEnvStat -> %d", getEnvStat);
    }
}

/**
 * Callback a client can register, to hear about errors replicating individual documents.
 *
 * @param repl
 * @param pushing
 * @param docID
 * @param error
 * @param transient
 * @param ctx
 */
static void documentErrorCallback(C4Replicator *repl, bool pushing, C4String docID, C4Error error,
                                  bool transient, void *ctx) {
    char message[256];
    c4error_getMessageC(error, message, sizeof(message));
    LOGI("[NATIVE] C4Replicator.documentErrorCallback() repl -> 0x%p, %s, %s", repl,
         (pushing ? "pushing" : "pulling"), message);

    JNIEnv *env = NULL;
    jint getEnvStat = gJVM->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
    if (getEnvStat == JNI_OK) {
        env->CallStaticVoidMethod(cls_C4Replicator,
                                  m_C4Replicator_documentErrorCallback,
                                  (jlong) repl,
                                  pushing,
                                  toJString(env, docID),
                                  error.domain, error.code, error.internal_info, transient);
    } else if (getEnvStat == JNI_EDETACHED) {
        if (gJVM->AttachCurrentThread(&env, NULL) == 0) {
            env->CallStaticVoidMethod(cls_C4Replicator,
                                      m_C4Replicator_documentErrorCallback,
                                      (jlong) repl,
                                      pushing,
                                      toJString(env, docID),
                                      error.domain, error.code, error.internal_info, transient);
            if (gJVM->DetachCurrentThread() != 0)
                LOGE("doRequestClose(): Failed to detach the current thread from a Java VM");
        } else {
            LOGE("doRequestClose(): Failed to attaches the current thread to a Java VM");
        }
    } else {
        LOGE("doClose(): Failed to get the environment: getEnvStat -> %d", getEnvStat);
    }
}

/*
 * Class:     com_couchbase_litecore_C4Replicator
 * Method:    create
 * Signature: (JLjava/lang/String;Ljava/lang/String;ILjava/lang/String;Ljava/lang/String;JII[B)J
 */
JNIEXPORT jlong JNICALL Java_com_couchbase_litecore_C4Replicator_create(
        JNIEnv *env,
        jclass clazz,
        jlong jdb,
        jstring jscheme,
        jstring jhost,
        jint jport,
        jstring jpath,
        jstring jremoteDBName,
        jlong jotherLocalDB,
        jint jpush,
        jint jpull,
        jbyteArray joptions) {

    LOGI("[NATIVE] C4Replicator.create()");

    jstringSlice scheme(env, jscheme);
    jstringSlice host(env, jhost);
    jstringSlice path(env, jpath);
    jstringSlice remoteDBName(env, jremoteDBName);
    jbyteArraySlice options(env, joptions, false);

    C4Address c4Address = {};
    c4Address.scheme = scheme;
    c4Address.hostname = host;
    c4Address.port = jport;
    c4Address.path = path;

    C4ReplicatorParameters params = {};
    params.push = (C4ReplicatorMode) jpush;
    params.pull = (C4ReplicatorMode) jpull;
    params.optionsDictFleece = options;
    params.onStatusChanged = &statusChangedCallback;
    params.onDocumentError = &documentErrorCallback;
    params.callbackContext = NULL;

    C4Error error;
    C4Replicator *repl = c4repl_new((C4Database *) jdb,
                                    c4Address,
                                    remoteDBName,
                                    (C4Database *) jotherLocalDB,
                                    params,
                                    &error);
    if (!repl) {
        throwError(env, error);
        return 0;
    }

    LOGI("[NATIVE] C4Replicator.create() repl -> 0x%p", repl);

    return (jlong) repl;
}

/*
 * Class:     com_couchbase_litecore_C4Replicator
 * Method:    free
 * Signature: (J)V
 */
JNIEXPORT void JNICALL
Java_com_couchbase_litecore_C4Replicator_free(JNIEnv *env, jclass clazz, jlong repl) {
    LOGI("[NATIVE] C4Replicator.free() repl -> 0x%x", (unsigned int) repl);
    c4repl_free((C4Replicator *) repl);
}

/*
 * Class:     com_couchbase_litecore_C4Replicator
 * Method:    stop
 * Signature: (J)V
 */
JNIEXPORT void JNICALL
Java_com_couchbase_litecore_C4Replicator_stop(JNIEnv *env, jclass clazz, jlong repl) {
    LOGI("[NATIVE] C4Replicator.stop() repl -> 0x%x", (unsigned int) repl);
    c4repl_stop((C4Replicator *) repl);
}

/*
 * Class:     com_couchbase_litecore_C4Replicator
 * Method:    getStatus
 * Signature: (J)Lcom/couchbase/litecore/C4ReplicatorStatus;
 */
JNIEXPORT jobject JNICALL
Java_com_couchbase_litecore_C4Replicator_getStatus(JNIEnv *env, jclass clazz, jlong repl) {
    LOGI("[NATIVE] C4Replicator.getStatus() repl -> 0x%x", (unsigned int) repl);
    C4ReplicatorStatus status = c4repl_getStatus((C4Replicator *) repl);
    return toJavaObject(env, status);
}

/*
 * Class:     com_couchbase_litecore_C4Replicator
 * Method:    getResponseHeaders
 * Signature: (J)[B
 */
JNIEXPORT jbyteArray JNICALL
Java_com_couchbase_litecore_C4Replicator_getResponseHeaders(JNIEnv *env, jclass clazz, jlong repl) {
    LOGI("[NATIVE] C4Replicator.getResponseHeaders() repl -> 0x%x", (unsigned int) repl);
    C4Slice s = c4repl_getResponseHeaders((C4Replicator *) repl);
    jbyteArray res = toJByteArray(env, s);
    c4slice_free(s);
    return res;
}

/*
 * Class:     com_couchbase_litecore_C4Replicator
 * Method:    mayBeTransient
 * Signature: (III)Z
 */
JNIEXPORT jboolean JNICALL
Java_com_couchbase_litecore_C4Replicator_mayBeTransient(JNIEnv *env, jclass clazz,
                                                        jint domain, jint code, jint ii) {
    C4Error c4Error = {.domain = (C4ErrorDomain) domain, .code= code, .internal_info = ii};
    return c4error_mayBeTransient(c4Error);
}

/*
 * Class:     com_couchbase_litecore_C4Replicator
 * Method:    mayBeNetworkDependent
 * Signature: (III)Z
 */
JNIEXPORT jboolean JNICALL
Java_com_couchbase_litecore_C4Replicator_mayBeNetworkDependent(JNIEnv *env, jclass clazz,
                                                               jint domain, jint code, jint ii) {
    C4Error c4Error = {.domain = (C4ErrorDomain) domain, .code= code, .internal_info = ii};
    return c4error_mayBeNetworkDependent(c4Error);
}