//
//  native_document.cpp
//  CBForest
//
//  Created by Jens Alfke on 9/11/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//

#include "com_couchbase_cbforest_Document.h"
#include "native_glue.hh"
#include "c4Document.h"
#include <vector>

using namespace forestdb;
using namespace forestdb::jni;


static jfieldID kField_Flags;
static jfieldID kField_DocID;
static jfieldID kField_RevID;
static jfieldID kField_Sequence;
static jfieldID kField_SelectedRevID;
static jfieldID kField_SelectedRevFlags;
static jfieldID kField_SelectedSequence;
static jfieldID kField_SelectedBody;


bool forestdb::jni::initDocument(JNIEnv *env) {
    jclass documentClass = env->FindClass("com/couchbase/cbforest/Document");
    if (!documentClass)
        return false;
    kField_Flags = env->GetFieldID(documentClass, "_flags", "I");
    kField_DocID = env->GetFieldID(documentClass, "_docID", "Ljava/lang/String;");
    kField_RevID = env->GetFieldID(documentClass, "_revID", "Ljava/lang/String;");
    kField_Sequence = env->GetFieldID(documentClass, "_sequence", "J");
    kField_SelectedRevID = env->GetFieldID(documentClass, "_selectedRevID", "Ljava/lang/String;");
    kField_SelectedRevFlags = env->GetFieldID(documentClass, "_selectedRevFlags", "I");
    kField_SelectedSequence = env->GetFieldID(documentClass, "_selectedSequence", "J");
    kField_SelectedBody = env->GetFieldID(documentClass, "_selectedBody", "[B");
    return kField_Flags && kField_RevID && kField_SelectedRevID
        && kField_SelectedRevFlags && kField_SelectedSequence && kField_SelectedBody;
}

// Updates the _docID field of the Java Document object
static void updateDocID(JNIEnv *env, jobject self, C4Document *doc) {
    env->SetObjectField(self, kField_DocID, toJString(env, doc->docID));
}

// Updates the _revID and _flags fields of the Java Document object
static void updateRevIDAndFlags
(JNIEnv *env, jobject self, C4Document *doc) {
    env->SetObjectField(self, kField_RevID, toJString(env, doc->revID));
    env->SetLongField  (self, kField_Sequence, doc->sequence);
    env->SetIntField   (self, kField_Flags, doc->flags);
}

// Updates the "_selectedXXXX" fields of the Java Document object
static void updateSelection
(JNIEnv *env, jobject self, C4Document *doc, bool withBody =false) {
    auto sel = &doc->selectedRev;
    env->SetObjectField(self, kField_SelectedRevID,    toJString(env, sel->revID));
    env->SetLongField  (self, kField_SelectedSequence, sel->sequence);
    env->SetIntField   (self, kField_SelectedRevFlags, sel->flags);
    if(withBody)
        env->SetObjectField(self, kField_SelectedBody, toJByteArray(env, sel->body));
    else
        env->SetObjectField(self, kField_SelectedBody, NULL);
}


JNIEXPORT jlong JNICALL Java_com_couchbase_cbforest_Document_init
(JNIEnv *env, jobject self, jlong dbHandle, jstring jdocID, jboolean mustExist)
{
    jstringSlice docID(env, jdocID);
    C4Error error;
    C4Document *doc = c4doc_get((C4Database*)dbHandle, docID, mustExist, &error);
    if (!doc) {
        throwError(env, error);
        return 0;
    }
    updateRevIDAndFlags(env, self, doc);
    updateSelection(env, self, doc, true);
    return (jlong)doc;
}

JNIEXPORT jlong JNICALL Java_com_couchbase_cbforest_Document_initWithSequence
        (JNIEnv *env, jobject self, jlong dbHandle, jlong sequence)
{
    C4Error error;
    C4Document *doc = c4doc_getBySequence((C4Database*)dbHandle, sequence, &error);
    if (!doc) {
        throwError(env, error);
        return 0;
    }
    updateDocID(env, self, doc);
    updateRevIDAndFlags(env, self, doc);
    updateSelection(env, self, doc, true);
    return (jlong)doc;
}

JNIEXPORT jstring JNICALL Java_com_couchbase_cbforest_Document_initWithDocHandle
(JNIEnv *env, jobject self, jlong docHandle)
{
    auto doc = (C4Document*)docHandle;
    updateRevIDAndFlags(env, self, doc);
    if(c4doc_selectCurrentRevision(doc))
        updateSelection(env, self, doc);
    return toJString(env, doc->docID);
}
JNIEXPORT jboolean JNICALL Java_com_couchbase_cbforest_Document_hasRevisionBody
        (JNIEnv *env, jclass clazz, jlong docHandle)
{
    return c4doc_hasRevisionBody((C4Document *) docHandle);
}

JNIEXPORT jint JNICALL Java_com_couchbase_cbforest_Document_purgeRevision
        (JNIEnv *env, jclass clazz, jlong docHandle, jstring jrevid){
    auto doc = (C4Document *) docHandle;
    jstringSlice revID(env, jrevid);
    C4Error error;
    int num = c4doc_purgeRevision(doc, revID, &error);
    if (num == -1)
        throwError(env, error);
    return num;
}


JNIEXPORT void JNICALL Java_com_couchbase_cbforest_Document_free
(JNIEnv *env, jclass clazz, jlong docHandle)
{
    c4doc_free((C4Document*)docHandle);
}


JNIEXPORT jstring JNICALL Java_com_couchbase_cbforest_Document_getType
(JNIEnv *env, jclass clazz, jlong docHandle) {
    return toJString(env, c4doc_getType((C4Document*)docHandle));
}


JNIEXPORT void JNICALL Java_com_couchbase_cbforest_Document_setType
(JNIEnv *env, jclass clazz, jlong docHandle, jstring jtype)
{
    jstringSlice type(env, jtype);
    C4Error error;
    if (!c4doc_setType((C4Document*)docHandle, type, &error))
        throwError(env, error);
}



JNIEXPORT jboolean JNICALL Java_com_couchbase_cbforest_Document_selectRevID
(JNIEnv *env, jobject self, jlong docHandle, jstring jrevID, jboolean withBody)
{
    auto doc = (C4Document*)docHandle;
    jstringSlice revID(env, jrevID);
    C4Error error;
    bool ok = c4doc_selectRevision(doc, revID, withBody, &error);
    if (ok || error.domain == HTTPDomain)
        updateSelection(env, self, doc);
    else
        throwError(env, error);
    return ok;
}


JNIEXPORT jboolean JNICALL Java_com_couchbase_cbforest_Document_selectCurrentRev
(JNIEnv *env, jobject self, jlong docHandle)
{
    auto doc = (C4Document*)docHandle;
    bool ok = c4doc_selectCurrentRevision(doc);
    updateSelection(env, self, doc);
    return ok;
}


JNIEXPORT jboolean JNICALL Java_com_couchbase_cbforest_Document_selectParentRev
(JNIEnv *env, jobject self, jlong docHandle)
{
    auto doc = (C4Document*)docHandle;
    bool ok = c4doc_selectParentRevision(doc);
    updateSelection(env, self, doc);
    return ok;
}


JNIEXPORT jboolean JNICALL Java_com_couchbase_cbforest_Document_selectNextRev
(JNIEnv *env, jobject self, jlong docHandle)
{
    auto doc = (C4Document*)docHandle;
    bool ok = c4doc_selectNextRevision(doc);
    updateSelection(env, self, doc);
    return ok;
}


JNIEXPORT jboolean JNICALL Java_com_couchbase_cbforest_Document_selectNextLeaf
(JNIEnv *env, jobject self, jlong docHandle, jboolean includeDeleted, jboolean withBody)
{
    auto doc = (C4Document*)docHandle;
    C4Error error;
    bool ok = c4doc_selectNextLeafRevision(doc, includeDeleted, withBody, &error);
    if (ok || error.domain == HTTPDomain)  // 404 or 410 don't trigger exceptions
        updateSelection(env, self, doc, withBody);
    else
        throwError(env, error);
    return ok;
}


JNIEXPORT jbyteArray JNICALL Java_com_couchbase_cbforest_Document_readSelectedBody
(JNIEnv *env, jobject self, jlong docHandle)
{
    auto doc = (C4Document*)docHandle;
    C4Error error;
    if (!c4doc_loadRevisionBody(doc, &error)) {
        throwError(env, error);
        return NULL;
    }
    return toJByteArray(env, doc->selectedRev.body);
}


#pragma mark - INSERTING REVISIONS:


JNIEXPORT jboolean JNICALL Java_com_couchbase_cbforest_Document_insertRevision
(JNIEnv *env, jobject self, jlong docHandle,
 jstring jrevID, jbyteArray jbody,
 jboolean deleted, jboolean hasAtt,
 jboolean allowConflict)
{
    auto doc = (C4Document*)docHandle;
    int inserted;
    C4Error error;
    {
        jstringSlice revID(env, jrevID);
        jbyteArraySlice body(env, jbody, true); // critical
        inserted = c4doc_insertRevision(doc, revID, body, deleted, hasAtt, allowConflict, &error);
    }
    if (inserted < 0) {
        throwError(env, error);
        return false;
    }
    updateSelection(env, self, doc, true);
    updateRevIDAndFlags(env, self, doc);
    return (inserted > 0);
}


JNIEXPORT jint JNICALL Java_com_couchbase_cbforest_Document_insertRevisionWithHistory
(JNIEnv *env, jobject self, jlong docHandle,
 jbyteArray jbody,
 jboolean deleted, jboolean hasAtt,
 jobjectArray jhistory)
{
    auto doc = (C4Document*)docHandle;
    int inserted;
    C4Error error;
    {
        // Convert jhistory, a Java String[], to a C array of C4Slice:
        jsize n = env->GetArrayLength(jhistory);
        std::vector<C4Slice> history(n);
        std::vector<jstringSlice*> historyAlloc;
        for (jsize i = 0; i < n; i++) {
            jstring js = (jstring)env->GetObjectArrayElement(jhistory, i);
            jstringSlice *item = new jstringSlice(env, js);
            historyAlloc.push_back(item); // so its memory won't be freed
            history[i] = *item;
        }

        // Make sure the body will be released before releasing keeper.
        // Android ARM device caused memory access error when release jstringSlices (historyAlloc).
        // It seems the error is caused by `GetPrimitiveArrayCritical` and order of releasing memories
        // which are allocated through JNI methods.
        // https://github.com/couchbase/couchbase-lite-java-core/issues/793
        {
            jbyteArraySlice body(env, jbody, true); // critical
            inserted = c4doc_insertRevisionWithHistory(doc, body, deleted, hasAtt,
                                                       history.data(), n,
                                                       &error);
        }

        // release memory
        for (jsize i = 0; i < n; i++)
            delete historyAlloc.at(i);
        historyAlloc.clear();

    }
    if (inserted >= 0) {
        updateSelection(env, self, doc);
        updateRevIDAndFlags(env, self, doc);
    }
    else
        throwError(env, error);
    return inserted;
}


JNIEXPORT void JNICALL Java_com_couchbase_cbforest_Document_save
(JNIEnv *env, jobject self, jlong docHandle, jint maxRevTreeDepth) {
    auto doc = (C4Document*)docHandle;
    C4Error error;
    if (c4doc_save(doc, maxRevTreeDepth, &error))
        updateRevIDAndFlags(env, self, doc);
    else
        throwError(env, error);
}
