//
//  native_database.cc
//  CBForest
//
//  Created by Jens Alfke on 9/10/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//

#include "com_couchbase_cbforest_Database.h"
#include "native_glue.hh"
#include "c4Database.h"

using namespace forestdb::jni;


#pragma mark - DATABASE:


static jfieldID kHandleField;

static inline C4Database* getDbHandle(JNIEnv *env, jobject self) {
    return (C4Database*)env->GetLongField(self, kHandleField);
}

bool forestdb::jni::initDatabase(JNIEnv *env) {
    jclass dbClass = env->FindClass("Database");
    if (!dbClass)
        return false;
    kHandleField = env->GetFieldID(dbClass, "_handle", "L");
    return (kHandleField != NULL);
}


jlong JNICALL Java_com_couchbase_cbforest_Database__1open
(JNIEnv *env, jobject self, jstring jpath, jboolean readOnly)
{
    jstringSlice path(env, jpath);

    C4Error error;
    C4Database* db = c4db_open(path, readOnly, &error);
    if (!db)
        throwError(env, error);

    return (jlong)db;
}


JNIEXPORT void JNICALL Java_com_couchbase_cbforest_Database_free
(JNIEnv *env, jobject self)
{
    auto db = getDbHandle(env, self);
    if (db) {
        env->SetLongField(self, kHandleField, 0);
        C4Error error;
        if (!c4db_close(db, &error))
            throwError(env, error);
    }
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


JNIEXPORT jlong JNICALL Java_com_couchbase_cbforest_Database__1iterateChanges
(JNIEnv *env, jobject self, jlong since, jboolean bodies)
{
    C4Error error;
    C4DocEnumerator* e = c4db_enumerateChanges(getDbHandle(env, self), since, bodies, &error);
    if (!e)
        throwError(env, error);
    return (jlong)e;
}
