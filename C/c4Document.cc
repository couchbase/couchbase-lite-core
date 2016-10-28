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
#include "Database.hh"
#include "c4Document.h"
#include "c4Document+Fleece.h"
#include "c4Database.h"
#include "c4Private.h"

#include "Document.hh"
#include "Database.hh"
#include "SecureRandomize.hh"
#include "Fleece.hh"
#include "Fleece.h"


void c4doc_free(C4Document *doc) noexcept {
    delete (Document*)doc;
}


C4Document* c4doc_get(C4Database *database,
                      C4Slice docID,
                      bool mustExist,
                      C4Error *outError) noexcept
{
    return tryCatch<C4Document*>(outError, [&]{
        WITH_LOCK(database);
        auto doc = database->documentFactory().newDocumentInstance(docID);
        if (mustExist && !internal(doc)->exists()) {
            delete doc;
            doc = nullptr;
            recordError(LiteCoreDomain, kC4ErrorNotFound, outError);
        }
        return doc;
    });
}


C4Document* c4doc_getBySequence(C4Database *database,
                                C4SequenceNumber sequence,
                                C4Error *outError) noexcept
{
    return tryCatch<C4Document*>(outError, [&]{
        WITH_LOCK(database);
        auto doc = database->documentFactory().newDocumentInstance(database->defaultKeyStore().get(sequence));
        if (!internal(doc)->exists()) {
            delete doc;
            doc = nullptr;
            recordError(LiteCoreDomain, kC4ErrorNotFound, outError);
        }
        return doc;
    });
}


C4SliceResult c4doc_getType(C4Document *doc) noexcept {
    slice result = internal(doc)->type().copy();
    return {result.buf, result.size};
}

void c4doc_setType(C4Document *doc, C4Slice docType) noexcept {
    return internal(doc)->setType(docType);
}


#pragma mark - REVISIONS:


bool c4doc_selectRevision(C4Document* doc,
                          C4Slice revID,
                          bool withBody,
                          C4Error *outError) noexcept
{
    return tryCatch<bool>(outError, [&]{
        if (internal(doc)->selectRevision(revID, withBody))
            return true;
        recordError(LiteCoreDomain, kC4ErrorNotFound, outError);
        return false;
    });
}


bool c4doc_selectCurrentRevision(C4Document* doc) noexcept
{
    return internal(doc)->selectCurrentRevision();
}


C4SliceResult c4doc_detachRevisionBody(C4Document* doc) noexcept {
    alloc_slice result = internal(doc)->detachSelectedRevBody();
    result.dontFree();
    return {result.buf, result.size};
}


bool c4doc_loadRevisionBody(C4Document* doc, C4Error *outError) noexcept {
    return tryCatch<bool>(outError, [&]{
        if (internal(doc)->loadSelectedRevBodyIfAvailable())
            return true;
        recordError(LiteCoreDomain, kC4ErrorDeleted, outError);
        return false;
    });
}


bool c4doc_hasRevisionBody(C4Document* doc) noexcept {
    return tryCatch<bool>(nullptr, bind(&Document::hasRevisionBody, internal(doc)));
}


bool c4doc_selectParentRevision(C4Document* doc) noexcept {
    return internal(doc)->selectParentRevision();
}


bool c4doc_selectNextRevision(C4Document* doc) noexcept {
    return tryCatch<bool>(nullptr, bind(&Document::selectNextRevision, internal(doc)));
}


bool c4doc_selectNextLeafRevision(C4Document* doc,
                                  bool includeDeleted,
                                  bool withBody,
                                  C4Error *outError) noexcept
{
    return tryCatch<bool>(outError, [&]{
        if (internal(doc)->selectNextLeafRevision(includeDeleted)) {
            if (withBody)
                internal(doc)->loadSelectedRevBody();
            return true;
        }
        clearError(outError); // normal failure
        return false;
    });
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
                            C4Error *outError) noexcept
{
    if (!database->mustBeInTransaction(outError))
        return nullptr;
    Document *idoc = nullptr;
    try {
        alloc_slice newDocID;
        bool isNewDoc = (!docID.buf);
        if (isNewDoc) {
            newDocID = createDocUUID();
            docID = newDocID;
        }

        idoc = internal(database->documentFactory().newDocumentInstance(docID));
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
    return nullptr;
}


C4Document* c4doc_put(C4Database *database,
                      const C4DocPutRequest *rq,
                      size_t *outCommonAncestorIndex,
                      C4Error *outError) noexcept
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
                return nullptr;
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
        return nullptr;
    }
}


int32_t c4doc_purgeRevision(C4Document *doc,
                            C4Slice revID,
                            C4Error *outError) noexcept
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


#pragma mark - FLEECE-SPECIFIC:


using namespace fleece;


struct _FLEncoder* c4db_createFleeceEncoder(C4Database* db) noexcept {
    FLEncoder enc = FLEncoder_New();
    ((Encoder*)enc)->setSharedKeys(db->documentKeys());
    return enc;
}


C4SliceResult c4db_encodeJSON(C4Database *db, C4Slice jsonData, C4Error *outError) noexcept {
    return tryCatch<C4SliceResult>(outError, [&]{
        Encoder &enc = db->sharedEncoder();
        JSONConverter jc(enc);
        if (!jc.encodeJSON(jsonData)) {
            recordError(LiteCoreDomain, kC4ErrorCorruptData, outError);
            return C4SliceResult{};
        }
        slice result = enc.extractOutput().dontFree();
        return C4SliceResult{result.buf, result.size};
    });
}


C4SliceResult c4doc_bodyAsJSON(C4Document *doc, C4Error *outError) noexcept {
    return tryCatch<C4SliceResult>(outError, [&]{
        auto root = Value::fromTrustedData(doc->selectedRev.body);
        if (!root) {
            recordError(LiteCoreDomain, kC4ErrorCorruptData, outError);
            return C4SliceResult();
        }
        Database *db = c4Internal::internal(doc)->database();
        slice result = root->toJSON(db->documentKeys()).dontFree();
        return C4SliceResult{result.buf, result.size};
    });
}


FLDictKey c4db_initFLDictKey(C4Database *db, C4Slice string) noexcept {
    return FLDictKey_InitWithSharedKeys({string.buf, string.size},
                                        (FLSharedKeys)db->documentKeys());
}


FLSharedKeys c4db_getFLSharedKeys(C4Database *db) noexcept {
    return (FLSharedKeys)db->documentKeys();
}
