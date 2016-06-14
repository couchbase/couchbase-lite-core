//
//  native_database.cc
//  CBForest
//
//  Created by Jens Alfke on 9/10/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#include "com_couchbase_cbforest_Database.h"
#include "native_glue.hh"
#include "c4Database.h"
#include "c4Document.h"
#include "c4ExpiryEnumerator.h"
#include "LogInternal.hh"

#undef DEBUG_TERMINATION // Define this to install a C++ termination handler that dumps a backtrace
#ifdef DEBUG_TERMINATION
#include <execinfo.h>   // Not available in Linux or Windows?
#include <unistd.h>
#endif

using namespace cbforest::jni;


#pragma mark - DATABASE:


static jfieldID kHandleField;
static jmethodID kLoggerLogMethod;

static inline C4Database* getDbHandle(JNIEnv *env, jobject self) {
    return (C4Database*)env->GetLongField(self, kHandleField);
}

#ifdef DEBUG_TERMINATION
static void jniCBForestTerminateHandler() {
    fprintf(stderr, "***** CBFOREST UNCAUGHT C++ EXCEPTION *****\n");
    void* addrs[50];
    int n = backtrace(addrs, 50);
    backtrace_symbols_fd(addrs, n, STDERR_FILENO);
    fprintf(stderr, "***** CBFOREST NOW ABORTING *****\n");
    abort();
}
#endif

bool cbforest::jni::initDatabase(JNIEnv *env) {
#ifdef DEBUG_TERMINATION
    std::set_terminate(jniCBForestTerminateHandler);    // TODO: Take this out after debugging
#endif

    jclass dbClass = env->FindClass("com/couchbase/cbforest/Database");
    if (!dbClass)
        return false;
    kHandleField = env->GetFieldID(dbClass, "_handle", "J");
    if (!kHandleField)
        return false;
    jclass loggerClass = env->FindClass("com/couchbase/cbforest/Logger");
    if (!loggerClass)
        return false;
    kLoggerLogMethod = env->GetMethodID(loggerClass, "log", "(ILjava/lang/String;)V");
    if (!kLoggerLogMethod)
        return false;
    return true;
}


jlong JNICALL Java_com_couchbase_cbforest_Database__1open
(JNIEnv *env, jobject self, jstring jpath,
 jint flags, jint encryptionAlg, jbyteArray encryptionKey)
{
    jstringSlice path(env, jpath);
    C4EncryptionKey key;
    if (!getEncryptionKey(env, encryptionAlg, encryptionKey, &key))
        return 0;

    C4Error error;
    C4Database* db = c4db_open(path, (C4DatabaseFlags)flags, &key, &error);
    if (!db)
        throwError(env, error);

    return (jlong)db;
}

JNIEXPORT void JNICALL Java_com_couchbase_cbforest_Database_rekey
(JNIEnv *env, jobject self, jint encryptionAlg, jbyteArray encryptionKey){
    C4EncryptionKey key;
    if (!getEncryptionKey(env, encryptionAlg, encryptionKey, &key))
        return;

    auto db = getDbHandle(env, self);
    C4Error error;
    if(!c4db_rekey(db, &key, &error))
        throwError(env, error);
}

JNIEXPORT void JNICALL Java_com_couchbase_cbforest_Database_close
(JNIEnv *env, jobject self)
{
    auto db = getDbHandle(env, self);
    C4Error error;
    if (!c4db_close(db, &error))
        throwError(env, error);
}

JNIEXPORT void JNICALL Java_com_couchbase_cbforest_Database_free
(JNIEnv *env, jobject self)
{
    auto db = getDbHandle(env, self);
    env->SetLongField(self, kHandleField, 0);
    c4db_free(db);
    // Note: This is called only by the finalizer, so no further calls are possible.
}

JNIEXPORT void JNICALL Java_com_couchbase_cbforest_Database_compact
(JNIEnv *env, jobject self)
{
    auto db = getDbHandle(env, self);
    C4Error error;
    if (!c4db_compact(db, &error))
        throwError(env, error);
}

JNIEXPORT jlong JNICALL Java_com_couchbase_cbforest_Database_getDocumentCount
(JNIEnv *env, jobject self)
{
    return c4db_getDocumentCount(getDbHandle(env, self));
}


JNIEXPORT jlong JNICALL Java_com_couchbase_cbforest_Database_getLastSequence
(JNIEnv *env, jobject self)
{
    return c4db_getLastSequence(getDbHandle(env, self));
}


JNIEXPORT void JNICALL Java_com_couchbase_cbforest_Database_beginTransaction
(JNIEnv *env, jobject self)
{
    C4Error error;
    if (!c4db_beginTransaction(getDbHandle(env, self), &error))
        throwError(env, error);
}


JNIEXPORT void JNICALL Java_com_couchbase_cbforest_Database_endTransaction
(JNIEnv *env, jobject self, jboolean commit)
{
    C4Error error;
    if (!c4db_endTransaction(getDbHandle(env, self), commit, &error))
        throwError(env, error);
}


JNIEXPORT jboolean JNICALL Java_com_couchbase_cbforest_Database_isInTransaction
(JNIEnv *env, jobject self) {
    return c4db_isInTransaction(getDbHandle(env, self));
}


#pragma mark - LOGGING:


static jobject sLoggerRef;  // Global ref to the currently registered Logger instance

// NOTE: Log should not be used in critical (GetPrimitiveArrayCritical). It causes the memory error.
static void logCallback(C4LogLevel level, C4Slice message) {
    jobject logger = sLoggerRef;
    if (logger) {
        JNIEnv *env;
        if (gJVM->GetEnv((void**)&env, JNI_VERSION_1_2) == JNI_OK) {
            env->PushLocalFrame(1);
            jobject jmessage = toJString(env, message);
            env->CallVoidMethod(logger, kLoggerLogMethod, (jint)level, jmessage);
            env->PopLocalFrame(NULL);
        }
    }
}


JNIEXPORT void JNICALL Java_com_couchbase_cbforest_Database_setLogger
(JNIEnv *env, jclass klass, jobject logger, jint level)
{
    jobject oldLoggerRef = sLoggerRef;
    sLoggerRef = env->NewGlobalRef(logger);
    if (oldLoggerRef)
        env->DeleteGlobalRef(oldLoggerRef);
    c4log_register((C4LogLevel)level, &logCallback);
}

#pragma mark - DOCUMENTS:

JNIEXPORT void JNICALL Java_com_couchbase_cbforest_Database_purgeDoc
(JNIEnv *env, jclass clazz, jlong db, jstring jdocID)
{
    jstringSlice docID(env, jdocID);
    C4Error error;
    if(!c4db_purgeDoc((C4Database*)db, docID, &error))
        throwError(env, error);
}

#pragma mark - EXPIRATION:
/*
 * Class:     com_couchbase_cbforest_Database
 * Method:    expirationOfDoc
 * Signature: (JLjava/lang/String;)J
 */
JNIEXPORT jlong JNICALL Java_com_couchbase_cbforest_Database_expirationOfDoc
        (JNIEnv *env, jclass clazz, jlong dbHandle, jstring jdocID)
{
    jstringSlice docID(env, jdocID);
    return c4doc_getExpiration((C4Database*)dbHandle, docID);
}

/*
 * Class:     com_couchbase_cbforest_Database
 * Method:    setExpiration
 * Signature: (JLjava/lang/String;J)V
 */
JNIEXPORT void JNICALL Java_com_couchbase_cbforest_Database_setExpiration
        (JNIEnv *env, jclass clazz, jlong dbHandle, jstring jdocID, jlong jtimestamp)
{
    jstringSlice docID(env, jdocID);
    C4Error error;
    if(!c4doc_setExpiration((C4Database*)dbHandle, docID, jtimestamp, &error))
        throwError(env, error);
}

/*
 * Class:     com_couchbase_cbforest_Database
 * Method:    nextDocExpiration
 * Signature: (J)J
 */
JNIEXPORT jlong JNICALL Java_com_couchbase_cbforest_Database_nextDocExpiration
        (JNIEnv *env, jclass clazz, jlong dbHandle)
{
    return c4db_nextDocExpiration((C4Database*)dbHandle);
}

JNIEXPORT jobjectArray JNICALL Java_com_couchbase_cbforest_Database_purgeExpiredDocuments
        (JNIEnv *env, jclass clazz, jlong dbHandle)
{
    C4Error err;
    C4ExpiryEnumerator *e = c4db_enumerateExpired((C4Database *)dbHandle, &err);
    if (!e) {
        throwError(env, err);
        return 0;
    }

    std::vector<std::string> docIDs;
    while(c4exp_next(e, &err)) {
        C4SliceResult docID = c4exp_getDocID(e);
        std::string strDocID((char*)docID.buf, docID.size);
        C4Error docErr;
        if (!c4db_purgeDoc((C4Database *) dbHandle, docID, &docErr))
            Debug("Unable to purge expired doc: CBForest error %d/%d", docErr.domain, docErr.code);
        docIDs.push_back(strDocID);
        c4slice_free(docID);
    }
    if(err.code)
        Debug("Error enumerating expired docs: CBForest error %d/%d", err.domain,err.code);

    c4exp_purgeExpired(e, NULL);    // remove the expiration markers

    jobjectArray ret= (jobjectArray)env->NewObjectArray(
            docIDs.size(),
            env->FindClass("java/lang/String"),
            env->NewStringUTF(""));
    for (int i = 0; i < docIDs.size(); i++)
        env->SetObjectArrayElement(ret, i, env->NewStringUTF(docIDs[i].c_str()));

    if (e)
        c4exp_free(e);

    return ret;
}

#pragma mark - RAW DOCUMENTS:

JNIEXPORT void JNICALL Java_com_couchbase_cbforest_Database__1rawPut
(JNIEnv *env, jclass clazz, jlong db, jstring jstore, jstring jkey, jbyteArray jmeta, jbyteArray jbody)
{
    jstringSlice    store(env, jstore);
    jstringSlice    key(env, jkey);
    jbyteArraySlice meta(env, jmeta, true); // critical
    jbyteArraySlice body(env, jbody, true); // critical
    C4Error error;
    if(!c4raw_put((C4Database*)db, store, key, meta, body, &error))
        throwError(env, error);
}

JNIEXPORT jobjectArray JNICALL Java_com_couchbase_cbforest_Database__1rawGet
(JNIEnv *env, jclass clazz, jlong db, jstring jstore, jstring jkey) {
    // obtain raw document
    jstringSlice store(env, jstore);
    jstringSlice key(env, jkey);
    C4Error error;
    C4RawDocument *doc = c4raw_get((C4Database *)db, store, key, &error);
    if (doc == NULL) {
        throwError(env, error);
        // NOTE: throwError() is not same with throw Exception() of java.
        // Need to return, otherwise, following codes will be executed.
        return NULL;
    }

    // create two dimension array to return meta and body byte array
    jclass elemType = env->FindClass("[B");
    jobjectArray rows = env->NewObjectArray(2, elemType, NULL);
    if (rows != NULL) {
        env->SetObjectArrayElement(rows, 0, toJByteArray(env, doc->meta));
        env->SetObjectArrayElement(rows, 1, toJByteArray(env, doc->body));
    }

    // release raw document
    c4raw_free(doc);
    doc = NULL;

    return rows;
}

