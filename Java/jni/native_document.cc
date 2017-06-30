//
//  native_document.cpp
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 9/11/15.
//  Copyright (c) 2015-2016 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#include <c4.h>
#include "com_couchbase_litecore_Document.h"
#include "native_glue.hh"

using namespace litecore;
using namespace litecore::jni;

static jfieldID kField_Flags;
static jfieldID kField_DocID;
static jfieldID kField_RevID;
static jfieldID kField_Sequence;
static jfieldID kField_SelectedRevID;
static jfieldID kField_SelectedRevFlags;
static jfieldID kField_SelectedSequence;
static jfieldID kField_SelectedBody;

bool litecore::jni::initDocument(JNIEnv *env) {
    jclass documentClass = env->FindClass("com/couchbase/litecore/Document");
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
    env->SetLongField(self, kField_Sequence, doc->sequence);
    env->SetIntField(self, kField_Flags, doc->flags);
}

// Updates the "_selectedXXXX" fields of the Java Document object
static void updateSelection
        (JNIEnv *env, jobject self, C4Document *doc, bool withBody = false) {
    auto sel = &doc->selectedRev;

    jobject jRevID;
    if (c4SliceEqual(sel->revID, doc->revID)) {
        // Optimization -- assumes Java revID field is up-to-date (updateRevIDAndFlags was called)
        jRevID = env->GetObjectField(self, kField_RevID);
    } else {
        jRevID = toJString(env, sel->revID);
    }
    env->SetObjectField(self, kField_SelectedRevID, jRevID);
    env->SetLongField(self, kField_SelectedSequence, sel->sequence);
    env->SetIntField(self, kField_SelectedRevFlags, sel->flags);
    if (withBody)
        env->SetObjectField(self, kField_SelectedBody, toJByteArray(env, sel->body));
    else
        env->SetObjectField(self, kField_SelectedBody, nullptr);
}


JNIEXPORT jlong JNICALL Java_com_couchbase_litecore_Document_init
        (JNIEnv *env, jobject self, jlong dbHandle, jstring jdocID, jboolean mustExist) {
    jstringSlice docID(env, jdocID);
    C4Error error;
    C4Document *doc = c4doc_get((C4Database *) dbHandle, docID, mustExist, &error);
    if (!doc) {
        throwError(env, error);
        return 0;
    }
    updateRevIDAndFlags(env, self, doc);
    updateSelection(env, self, doc, true);
    return (jlong) doc;
}

JNIEXPORT jlong JNICALL Java_com_couchbase_litecore_Document_initWithSequence
        (JNIEnv *env, jobject self, jlong dbHandle, jlong sequence) {
    C4Error error;
    C4Document *doc = c4doc_getBySequence((C4Database *) dbHandle, sequence, &error);
    if (!doc) {
        throwError(env, error);
        return 0;
    }
    updateDocID(env, self, doc);
    updateRevIDAndFlags(env, self, doc);
    updateSelection(env, self, doc, true);
    return (jlong) doc;
}

JNIEXPORT jstring JNICALL Java_com_couchbase_litecore_Document_initWithDocHandle
        (JNIEnv *env, jobject self, jlong docHandle) {
    auto doc = (C4Document *) docHandle;
    updateRevIDAndFlags(env, self, doc);
    updateSelection(env, self, doc);
    return toJString(env, doc->docID);
}

JNIEXPORT jboolean JNICALL Java_com_couchbase_litecore_Document_hasRevisionBody
        (JNIEnv *env, jclass clazz, jlong docHandle) {
    return c4doc_hasRevisionBody((C4Document *) docHandle);
}

JNIEXPORT jint JNICALL Java_com_couchbase_litecore_Document_purgeRevision
        (JNIEnv *env, jclass clazz, jlong docHandle, jstring jrevid) {
    auto doc = (C4Document *) docHandle;
    jstringSlice revID(env, jrevid);
    C4Error error;
    int num = c4doc_purgeRevision(doc, revID, &error);
    if (num == -1)
        throwError(env, error);
    return num;
}

/*
 * Class:     com_couchbase_litecore_Document
 * Method:    resolveConflict
 * Signature: (JLjava/lang/String;Ljava/lang/String;[B)Z
 */
JNIEXPORT jboolean JNICALL
Java_com_couchbase_litecore_Document_resolveConflict(JNIEnv *env, jclass clazz, jlong docHandle,
                                                     jstring jWinningRevID, jstring jLosingRevID,
                                                     jbyteArray jMergedBody) {
    C4Document *doc = (C4Document *) docHandle;
    jstringSlice winningRevID(env, jWinningRevID);
    jstringSlice losingRevID(env, jLosingRevID);
    jbyteArraySlice mergedBody(env, jMergedBody, false);
    C4Error error = {};
    bool ok = c4doc_resolveConflict(doc, winningRevID, losingRevID, mergedBody, &error);
    if (!ok)
        throwError(env, error);
    return ok;
}

JNIEXPORT void JNICALL Java_com_couchbase_litecore_Document_free
        (JNIEnv *env, jclass clazz, jlong docHandle) {
    c4doc_free((C4Document *) docHandle);
}

static bool isNotFoundError(C4Error error) {
    return error.domain == LiteCoreDomain && (error.code == kC4ErrorNotFound ||
                                              error.code == kC4ErrorDeleted);
}

JNIEXPORT jboolean JNICALL Java_com_couchbase_litecore_Document_selectRevID
        (JNIEnv *env, jobject self, jlong docHandle, jstring jrevID, jboolean withBody) {
    auto doc = (C4Document *) docHandle;
    jstringSlice revID(env, jrevID);
    C4Error error;
    bool ok = c4doc_selectRevision(doc, revID, withBody, &error);
    if (ok || isNotFoundError(error)) {
        updateSelection(env, self, doc);
    } else {
        throwError(env, error);
    }
    return ok;
}

JNIEXPORT jboolean JNICALL Java_com_couchbase_litecore_Document_selectCurrentRev
        (JNIEnv *env, jobject self, jlong docHandle) {
    auto doc = (C4Document *) docHandle;
    bool ok = c4doc_selectCurrentRevision(doc);
    updateSelection(env, self, doc);
    return ok;
}

JNIEXPORT jboolean JNICALL Java_com_couchbase_litecore_Document_selectParentRev
        (JNIEnv *env, jobject self, jlong docHandle) {
    auto doc = (C4Document *) docHandle;
    bool ok = c4doc_selectParentRevision(doc);
    updateSelection(env, self, doc);
    return ok;
}

JNIEXPORT jboolean JNICALL Java_com_couchbase_litecore_Document_selectNextRev
        (JNIEnv *env, jobject self, jlong docHandle) {
    auto doc = (C4Document *) docHandle;
    bool ok = c4doc_selectNextRevision(doc);
    updateSelection(env, self, doc);
    return ok;
}

JNIEXPORT jboolean JNICALL Java_com_couchbase_litecore_Document_selectNextLeaf
        (JNIEnv *env, jobject self, jlong docHandle, jboolean includeDeleted, jboolean withBody) {
    auto doc = (C4Document *) docHandle;
    C4Error error;
    bool ok = c4doc_selectNextLeafRevision(doc, includeDeleted, withBody, &error);
    if (ok || error.code == 0 || isNotFoundError(error))  // 404 or 410 don't trigger exceptions
        updateSelection(env, self, doc, withBody);
    else
        throwError(env, error);
    return ok;
}

/*
 * Class:     com_couchbase_litecore_Document
 * Method:    selectFirstPossibleAncestorOf
 * Signature: (JLjava/lang/String;)Z
 */
JNIEXPORT jboolean JNICALL
Java_com_couchbase_litecore_Document_selectFirstPossibleAncestorOf(JNIEnv *env, jobject self,
                                                                   jlong docHandle,
                                                                   jstring jRevID) {
    jstringSlice revID(env, jRevID);
    auto doc = (C4Document *) docHandle;
    bool ok = c4doc_selectFirstPossibleAncestorOf(doc, revID);
    updateSelection(env, self, doc);
    return ok;
}

/*
 * Class:     com_couchbase_litecore_Document
 * Method:    selectNextPossibleAncestorOf
 * Signature: (JLjava/lang/String;)Z
 */
JNIEXPORT jboolean JNICALL
Java_com_couchbase_litecore_Document_selectNextPossibleAncestorOf(JNIEnv *env, jobject self,
                                                                  jlong docHandle, jstring jRevID) {
    jstringSlice revID(env, jRevID);
    auto doc = (C4Document *) docHandle;
    bool ok = c4doc_selectNextPossibleAncestorOf(doc, revID);
    updateSelection(env, self, doc);
    return ok;
}
/*
 * Class:     com_couchbase_litecore_Document
 * Method:    selectCommonAncestorRevision
 * Signature: (JLjava/lang/String;Ljava/lang/String;)Z
 */
JNIEXPORT jboolean JNICALL Java_com_couchbase_litecore_Document_selectCommonAncestorRevision
        (JNIEnv *env, jobject self, jlong docHandle, jstring jRev1, jstring jRev2){
    jstringSlice rev1(env, jRev1);
    jstringSlice rev2(env, jRev2);
    auto doc = (C4Document *) docHandle;
    bool ok = c4doc_selectCommonAncestorRevision((C4Document *) docHandle, rev1, rev2);
    updateSelection(env, self, doc);
    return ok;
}

JNIEXPORT jbyteArray JNICALL Java_com_couchbase_litecore_Document_readSelectedBody
        (JNIEnv *env, jobject self, jlong docHandle) {
    auto doc = (C4Document *) docHandle;
    C4Error error;
    if (!c4doc_loadRevisionBody(doc, &error)) {
        throwError(env, error);
        return nullptr;
    }
    return toJByteArray(env, doc->selectedRev.body);
}

JNIEXPORT void JNICALL Java_com_couchbase_litecore_Document_save
        (JNIEnv *env, jobject self, jlong docHandle, jint maxRevTreeDepth) {
    auto doc = (C4Document *) docHandle;
    C4Error error;
    if (c4doc_save(doc, maxRevTreeDepth, &error))
        updateRevIDAndFlags(env, self, doc);
    else
        throwError(env, error);
}
