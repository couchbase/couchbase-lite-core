//
// native_c4socket.cc
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
#include <c4Socket.h>
#include "com_couchbase_litecore_C4Socket.h"
#include "native_glue.hh"
#include "logging.h"

using namespace litecore;
using namespace litecore::jni;

// ----------------------------------------------------------------------------
// Callback method IDs to C4Socket
// ----------------------------------------------------------------------------
// C4Socket
static jclass cls_C4Socket;                   // global reference to C4Socket
static jmethodID m_C4Socket_open;             // callback method for C4Socket.open(...)
static jmethodID m_C4Socket_write;            // callback method for C4Socket.write(...)
static jmethodID m_C4Socket_completedReceive; // callback method for C4Socket.completedReceive(...)
static jmethodID m_C4Socket_close;            // callback method for C4Socket.close(...)
static jmethodID m_C4Socket_requestClose;     // callback method for C4Socket.requestClose(...)

bool litecore::jni::initC4Socket(JNIEnv *env) {
    // Find C4Socket class and static methods for callback
    {
        jclass localClass = env->FindClass("com/couchbase/litecore/C4Socket");
        if (!localClass)
            return false;

        cls_C4Socket = reinterpret_cast<jclass>(env->NewGlobalRef(localClass));
        if (!cls_C4Socket)
            return false;

        m_C4Socket_open = env->GetStaticMethodID(
                cls_C4Socket,
                "open",
                "(JLjava/lang/String;Ljava/lang/String;ILjava/lang/String;[B)V");
        if (!m_C4Socket_open)
            return false;

        m_C4Socket_write = env->GetStaticMethodID(cls_C4Socket,
                                                  "write",
                                                  "(J[B)V");
        if (!m_C4Socket_write)
            return false;

        m_C4Socket_completedReceive = env->GetStaticMethodID(cls_C4Socket,
                                                             "completedReceive",
                                                             "(JJ)V");
        if (!m_C4Socket_completedReceive)
            return false;

        m_C4Socket_close = env->GetStaticMethodID(cls_C4Socket,
                                                  "close",
                                                  "(J)V");
        if (!m_C4Socket_close)
            return false;

        m_C4Socket_requestClose = env->GetStaticMethodID(cls_C4Socket,
                                                         "requestClose",
                                                         "(JILjava/lang/String;)V");
        if (!m_C4Socket_requestClose)
            return false;
    }

    return true;
}

// ----------------------------------------------------------------------------
// C4SocketFactory implementation
// ----------------------------------------------------------------------------
static void doOpen(C4Socket *s, const C4Address *addr, C4Slice optionsFleece) {
    //LOGI("doOpen() s -> 0x%x", s);
    JNIEnv *env = NULL;
    jint getEnvStat = gJVM->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
    if (getEnvStat == JNI_OK) {
        env->CallStaticVoidMethod(cls_C4Socket,
                                  m_C4Socket_open,
                                  (jlong) s,
                                  toJString(env, addr->scheme),
                                  toJString(env, addr->hostname),
                                  addr->port,
                                  toJString(env, addr->path),
                                  toJByteArray(env, optionsFleece));
    } else if (getEnvStat == JNI_EDETACHED) {
        if (gJVM->AttachCurrentThread(&env, NULL) == 0) {
            env->CallStaticVoidMethod(cls_C4Socket,
                                      m_C4Socket_open,
                                      (jlong) s,
                                      toJString(env, addr->scheme),
                                      toJString(env, addr->hostname),
                                      addr->port,
                                      toJString(env, addr->path),
                                      toJByteArray(env, optionsFleece));
            if (gJVM->DetachCurrentThread() != 0) {
                //LOGE("doRequestClose(): Failed to detach the current thread from a Java VM");
            }
        } else {
            //LOGE("doRequestClose(): Failed to attaches the current thread to a Java VM");
        }
    } else {
        //LOGE("doWrite(): Failed to get the environment: getEnvStat -> %d", getEnvStat);
    }
}

static void doWrite(C4Socket *s, C4SliceResult allocatedData) {
    //LOGI("doWrite() s -> 0x%x data.size -> %d", s, allocatedData.size);
    JNIEnv *env = NULL;
    jint getEnvStat = gJVM->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
    if (getEnvStat == JNI_OK) {
        env->CallStaticVoidMethod(cls_C4Socket,
                                  m_C4Socket_write,
                                  (jlong) s,
                                  toJByteArray(env, allocatedData));
    } else if (getEnvStat == JNI_EDETACHED) {
        if (gJVM->AttachCurrentThread(&env, NULL) == 0) {
            env->CallStaticVoidMethod(cls_C4Socket,
                                      m_C4Socket_write,
                                      (jlong) s,
                                      toJByteArray(env, allocatedData));
            if (gJVM->DetachCurrentThread() != 0) {
                //LOGE("doRequestClose(): Failed to detach the current thread from a Java VM");
            }
        } else {
            //LOGE("doRequestClose(): Failed to attaches the current thread to a Java VM");
        }
    } else {
        //LOGE("doWrite(): Failed to get the environment: getEnvStat -> %d", getEnvStat);
    }
}

static void doCompletedReceive(C4Socket *s, size_t byteCount) {
    //LOGI("doCompletedReceive() s -> 0x%x byteCount -> %ld", s, byteCount);
    JNIEnv *env = NULL;
    jint getEnvStat = gJVM->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
    if (getEnvStat == JNI_OK) {
        env->CallStaticVoidMethod(cls_C4Socket,
                                  m_C4Socket_completedReceive,
                                  (jlong) s,
                                  (jlong) byteCount);
    } else if (getEnvStat == JNI_EDETACHED) {
        if (gJVM->AttachCurrentThread(&env, NULL) == 0) {
            env->CallStaticVoidMethod(cls_C4Socket,
                                      m_C4Socket_completedReceive,
                                      (jlong) s,
                                      (jlong) byteCount);
            if (gJVM->DetachCurrentThread() != 0) {
                //LOGE("doRequestClose(): Failed to detach the current thread from a Java VM");
            }
        } else {
            //LOGE("doRequestClose(): Failed to attaches the current thread to a Java VM");
        }
    } else {
        //LOGE("doCompletedReceive(): Failed to get the environment: getEnvStat -> %d", getEnvStat);
    }
}

static void doClose(C4Socket *s) {
    ////LOGI("doClose() s -> 0x%x", s);
    JNIEnv *env = NULL;
    jint getEnvStat = gJVM->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
    if (getEnvStat == JNI_OK) {
        env->CallStaticVoidMethod(cls_C4Socket, m_C4Socket_close, (jlong) s);
    } else if (getEnvStat == JNI_EDETACHED) {
        if (gJVM->AttachCurrentThread(&env, NULL) == 0) {
            env->CallStaticVoidMethod(cls_C4Socket, m_C4Socket_close, (jlong) s);
            if (gJVM->DetachCurrentThread() != 0) {
                //LOGE("doRequestClose(): Failed to detach the current thread from a Java VM");
            }
        } else {
            //LOGE("doRequestClose(): Failed to attaches the current thread to a Java VM");
        }
    } else {
        //LOGE("doClose(): Failed to get the environment: getEnvStat -> %d", getEnvStat);
    }
}

static void doRequestClose(C4Socket *s, int status, C4String message) {
    //LOGI("doRequestClose() s -> 0x%x status -> %d", s, status);
    JNIEnv *env = NULL;
    jint getEnvStat = gJVM->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
    if (getEnvStat == JNI_OK) {
        env->CallStaticVoidMethod(cls_C4Socket,
                                  m_C4Socket_requestClose,
                                  (jlong) s,
                                  (jint) status,
                                  toJString(env, message));
    } else if (getEnvStat == JNI_EDETACHED) {
        if (gJVM->AttachCurrentThread(&env, NULL) == 0) {
            env->CallStaticVoidMethod(cls_C4Socket,
                                      m_C4Socket_requestClose,
                                      (jlong) s,
                                      (jint) status,
                                      toJString(env, message));
            if (gJVM->DetachCurrentThread() != 0) {
                //LOGE("doRequestClose(): Failed to detach the current thread from a Java VM");
            }
        } else {
            //LOGE("doRequestClose(): Failed to attaches the current thread to a Java VM");
        }
    } else {
        //LOGE("doRequestClose(): Failed to get the environment: getEnvStat -> %d", getEnvStat);
    }
}

// ----------------------------------------------------------------------------
// com_couchbase_litecore_C4Socket
// ----------------------------------------------------------------------------

/*
 * Class:     com_couchbase_litecore_C4Socket
 * Method:    registerFactory
 * Signature: ()V
 */
JNIEXPORT void JNICALL
Java_com_couchbase_litecore_C4Socket_registerFactory(JNIEnv *env, jclass clazz) {
    //LOGI("[NATIVE] registerFactory()");
    C4SocketFactory factory = {
            .providesWebSockets = true,
            .open = &doOpen,
            .write = &doWrite,
            .completedReceive = &doCompletedReceive,
            .requestClose = &doRequestClose
            //.close = &doClose
    };
    c4socket_registerFactory(factory);
}
/*
 * Class:     com_couchbase_litecore_C4Socket
 * Method:    gotHTTPResponse
 * Signature: (JI[B)V
 */
JNIEXPORT void JNICALL
Java_com_couchbase_litecore_C4Socket_gotHTTPResponse(JNIEnv *env, jclass clazz, jlong socket,
                                                     jint httpStatus,
                                                     jbyteArray jresponseHeadersFleece) {
    jbyteArraySlice responseHeadersFleece(env, jresponseHeadersFleece, false);
    c4socket_gotHTTPResponse((C4Socket *) socket, httpStatus, responseHeadersFleece);
}
/*
 * Class:     com_couchbase_litecore_C4Socket
 * Method:    opened
 * Signature: (J)V
 */
JNIEXPORT void JNICALL
Java_com_couchbase_litecore_C4Socket_opened(JNIEnv *env, jclass clazz, jlong socket) {
    //LOGI("[NATIVE] opened() socket -> 0x%x", socket);
    c4socket_opened((C4Socket *) socket);
}

/*
 * Class:     com_couchbase_litecore_C4Socket
 * Method:    closed
 * Signature: (JIILjava/lang/String;)V
 */
JNIEXPORT void JNICALL
Java_com_couchbase_litecore_C4Socket_closed(JNIEnv *env, jclass clazz,
                                            jlong socket,
                                            jint domain,
                                            jint code,
                                            jstring message) {
    //LOGI("[NATIVE] closed() socket -> 0x%x", socket);
    jstringSlice sliceMessage(env, message);
    C4Error error = c4error_make((C4ErrorDomain) domain, code, sliceMessage);
    c4socket_closed((C4Socket *) socket, error);
}

/*
 * Class:     com_couchbase_litecore_C4Socket
 * Method:    closeRequested
 * Signature: (JILjava/lang/String;)V
 */
JNIEXPORT void JNICALL
Java_com_couchbase_litecore_C4Socket_closeRequested(JNIEnv *env, jclass clazz,
                                                    jlong socket,
                                                    jint status,
                                                    jstring jmessage) {
    //LOGI("[NATIVE] closeRequested() socket -> 0x%x", socket);
    jstringSlice message(env, jmessage);
    c4socket_closeRequested((C4Socket *) socket, (int) status, message);
}

/*
 * Class:     com_couchbase_litecore_C4Socket
 * Method:    completedWrite
 * Signature: (JJ)V
 */
JNIEXPORT void JNICALL
Java_com_couchbase_litecore_C4Socket_completedWrite(JNIEnv *env, jclass clazz,
                                                    jlong socket,
                                                    jlong byteCount) {
    //LOGI("[NATIVE] completedWrite() socket -> 0x%x", socket);
    c4socket_completedWrite((C4Socket *) socket, (size_t) byteCount);
}

/*
 * Class:     com_couchbase_litecore_C4Socket
 * Method:    received
 * Signature: (J[B)V
 */
JNIEXPORT void JNICALL
Java_com_couchbase_litecore_C4Socket_received(JNIEnv *env, jclass clazz,
                                              jlong socket,
                                              jbyteArray jdata) {
    //LOGI("[NATIVE] received() socket -> 0x%x", socket);
    jbyteArraySlice data(env, jdata, false);
    c4socket_received((C4Socket *) socket, data);
}
