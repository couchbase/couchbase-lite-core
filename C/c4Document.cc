//
//  c4Document.cc
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 11/6/15.
//  Copyright (c) 2015-2016 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#define NOMINMAX
#include "c4Internal.hh"
#include "c4DatabaseInternal.hh"
#include "c4Document.h"
#include "c4Database.h"
#include "c4Private.h"

#include "c4DocInternal.hh"
#include "SecureRandomize.hh"


void c4doc_free(C4Document *doc) {
    delete (C4DocumentInternal*)doc;
}


C4Document* c4doc_get(C4Database *database,
                      C4Slice docID,
                      bool mustExist,
                      C4Error *outError)
{
    try {
        WITH_LOCK(database);
        auto doc = database->newDocumentInstance(docID);
        if (mustExist && !internal(doc)->exists()) {
            delete doc;
            doc = NULL;
            recordError(LiteCoreDomain, kC4ErrorNotFound, outError);
        }
        
        
        return doc;
    } catchError(outError);
    return NULL;
}


C4Document* c4doc_getBySequence(C4Database *database,
                                C4SequenceNumber sequence,
                                C4Error *outError)
{
    try {
        WITH_LOCK(database);
        auto doc = database->newDocumentInstance(database->defaultKeyStore().get(sequence));
        if (!internal(doc)->exists()) {
            delete doc;
            doc = NULL;
            recordError(LiteCoreDomain, kC4ErrorNotFound, outError);
        }
        return doc;
    } catchError(outError);
    return NULL;
}


C4SliceResult c4doc_getType(C4Document *doc) {
    slice result = internal(doc)->type().copy();
    return {result.buf, result.size};
}

void c4doc_setType(C4Document *doc, C4Slice docType) {
    return internal(doc)->setType(docType);
}


#pragma mark - REVISIONS:


bool c4doc_selectRevision(C4Document* doc,
                          C4Slice revID,
                          bool withBody,
                          C4Error *outError)
{
    try {
        if (internal(doc)->selectRevision(revID, withBody))
            return true;
        recordError(LiteCoreDomain, kC4ErrorNotFound, outError);
    } catchError(outError);
    return false;
}


bool c4doc_selectCurrentRevision(C4Document* doc)
{
    return internal(doc)->selectCurrentRevision();
}


bool c4doc_loadRevisionBody(C4Document* doc, C4Error *outError) {
    try {
        if (internal(doc)->loadSelectedRevBodyIfAvailable())
            return true;
        recordError(LiteCoreDomain, kC4ErrorDeleted, outError);
    } catchError(outError);
    return false;
}


bool c4doc_hasRevisionBody(C4Document* doc) {
    try {
        return internal(doc)->hasRevisionBody();
    } catchError(NULL);
    return false;
}


bool c4doc_selectParentRevision(C4Document* doc) {
    return internal(doc)->selectParentRevision();
}


bool c4doc_selectNextRevision(C4Document* doc) {
    return internal(doc)->selectNextRevision();
}


bool c4doc_selectNextLeafRevision(C4Document* doc,
                                  bool includeDeleted,
                                  bool withBody,
                                  C4Error *outError)
{
    try {
        if (internal(doc)->selectNextLeafRevision(includeDeleted, withBody))
            return true;
        clearError(outError); // normal failure
    } catchError(outError);
    return false;
}


#pragma mark - SAVING:


static alloc_slice createDocUUID() {
#if SECURE_RANDOMIZE_AVAILABLE
    static const char kBase64[65] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
                                    "0123456789-_";
    const unsigned kLength = 22; // 22 random base64 chars = 132 bits of entropy
    uint8_t r[kLength];
    SecureRandomize({r, sizeof(r)});

    alloc_slice docIDSlice(1+kLength);
    char *docID = (char*)docIDSlice.buf;
    docID[0] = '-';
    for (unsigned i = 0; i < kLength; ++i)
        docID[i+1] = kBase64[r[i] % 64];
    return docIDSlice;
#else
    error::_throw(error::Unimplemented);
#endif
}


// Finds a document for a Put of a _new_ revision, and selects the existing parent revision.
// After this succeeds, you can call c4doc_insertRevision and then c4doc_save.
C4Document* c4doc_getForPut(C4Database *database,
                            C4Slice docID,
                            C4Slice parentRevID,
                            bool deleting,
                            bool allowConflict,
                            C4Error *outError)
{
    if (!database->mustBeInTransaction(outError))
        return NULL;
    C4DocumentInternal *idoc = NULL;
    try {
        alloc_slice newDocID;
        bool isNewDoc = (!docID.buf);
        if (isNewDoc) {
            newDocID = createDocUUID();
            docID = newDocID;
        }

        idoc = internal(database->newDocumentInstance(docID));
        int code = 0;

        if (parentRevID.buf) {
            // Updating an existing revision; make sure it exists and is a leaf:
            if (!idoc->exists())
                code = kC4ErrorNotFound;
            else if (!idoc->selectRevision(parentRevID, false))
                code = allowConflict ? kC4ErrorNotFound : kC4ErrorConflict;
            else if (!allowConflict && !(idoc->selectedRev.flags & kRevLeaf))
                code = kC4ErrorConflict;
        } else {
            // No parent revision given:
            if (deleting) {
                // Didn't specify a revision to delete: NotFound or a Conflict, depending
                code = ((idoc->flags & kExists) ?kC4ErrorConflict :kC4ErrorNotFound);
            } else if ((idoc->flags & kExists) && !(idoc->selectedRev.flags & kDeleted)) {
                // If doc exists, current rev must be a deletion or there will be a conflict:
                code = kC4ErrorConflict;
            }
        }

        if (code)
            recordError(LiteCoreDomain, code, outError);
        else
            return idoc;

    } catchError(outError)
    delete idoc;
    return NULL;
}


C4Document* c4doc_put(C4Database *database,
                      const C4DocPutRequest *rq,
                      size_t *outCommonAncestorIndex,
                      C4Error *outError)
{
    if (!database->mustBeInTransaction(outError))
        return nullptr;
    int commonAncestorIndex;
    C4Document *doc = nullptr;
    try {
        if (rq->existingRevision) {
            // Existing revision:
            if (rq->docID.size == 0 || rq->historyCount == 0)
                error::_throw(error::InvalidParameter);
            doc = c4doc_get(database, rq->docID, false, outError);
            if (!doc)
                return NULL;
            commonAncestorIndex = internal(doc)->putExistingRevision(*rq);

        } else {
            // New revision:
            C4Slice parentRevID;
            if (rq->historyCount == 1)
                parentRevID = rq->history[0];
            else if (rq->historyCount > 1)
                error::_throw(error::InvalidParameter);
            doc = c4doc_getForPut(database, rq->docID, parentRevID, rq->deletion, rq->allowConflict,
                                  outError);
            if (!doc)
                return nullptr;
            commonAncestorIndex = internal(doc)->putNewRevision(*rq) ? 1 : 0;
        }

        if (outCommonAncestorIndex)
            *outCommonAncestorIndex = commonAncestorIndex;
        return doc;

    } catchError(outError) {
        c4doc_free(doc);
        return NULL;
    }
}


int32_t c4doc_purgeRevision(C4Document *doc,
                            C4Slice revID,
                            C4Error *outError)
{
    auto idoc = internal(doc);
    if (!idoc->mustBeInTransaction(outError))
        return -1;
    try {
        idoc->loadRevisions();
        return idoc->purgeRevision(revID);
    } catchError(outError)
    return -1;
}
