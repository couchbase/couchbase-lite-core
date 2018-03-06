//
// c4Document.cc
//
// Copyright (c) 2015 Couchbase, Inc All rights reserved.
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

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "c4Internal.hh"
#include "Database.hh"
#include "c4.h"
#include "c4Document+Fleece.h"
#include "c4Private.h"

#include "Document.hh"
#include "Database.hh"
#include "RevTree.hh"   // only for kDefaultRemoteID
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
        auto doc = database->documentFactory().newDocumentInstance(database->defaultKeyStore().get(sequence));
        if (!internal(doc)->exists()) {
            delete doc;
            doc = nullptr;
            recordError(LiteCoreDomain, kC4ErrorNotFound, outError);
        }
        return doc;
    });
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
    return sliceResult(internal(doc)->detachSelectedRevBody());
}


bool c4doc_loadRevisionBody(C4Document* doc, C4Error *outError) noexcept {
    return tryCatch<bool>(outError, [&]{
        if (internal(doc)->loadSelectedRevBody())
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


bool c4doc_selectFirstPossibleAncestorOf(C4Document* doc, C4Slice revID) noexcept {
    // LCOV_EXCL_START
    if (internal(doc)->database()->config.versioning != kC4RevisionTrees) {
        Warn("c4doc_selectFirstPossibleAncestorOf only works with revision trees");
        return false;
    }
    // LCOV_EXCL_STOP

    // Start at first (current) revision; return it if it's a candidate, else go to the next:
    c4doc_selectCurrentRevision(doc);
    auto generation = c4rev_getGeneration(revID);
    if (c4rev_getGeneration(doc->selectedRev.revID) < generation)
        return true;
    else
        return c4doc_selectNextPossibleAncestorOf(doc, revID);
}


bool c4doc_selectNextPossibleAncestorOf(C4Document* doc, C4Slice revID) noexcept {
    auto generation = c4rev_getGeneration(revID);
    while (c4doc_selectNextRevision(doc)) {
        // A possible ancestor is one with a lower generation number:
        if (c4rev_getGeneration(doc->selectedRev.revID) < generation)
            return true;
    }
    return false;
}


bool c4doc_selectCommonAncestorRevision(C4Document* doc, C4String rev1, C4String rev2) noexcept {
    return tryCatch<bool>(nullptr, [&]{
        return internal(doc)->selectCommonAncestorRevision(rev1, rev2);
    });
}


#pragma mark - REMOTE DATABASE REVISION TRACKING:


static const char * kRemoteDBURLsDoc = "remotes";


C4RemoteID c4db_getRemoteDBID(C4Database *db, C4String remoteAddress, bool canCreate,
                              C4Error *outError) C4API
{
    using namespace fleece;
    bool inTransaction = false;
    auto remoteID = tryCatch<C4RemoteID>(outError, [&]() {
        // Make two passes: In the first, just look up the "remotes" doc and look for an ID.
        // If the ID isn't found, then do a second pass where we either add the remote URL
        // or create the doc from scratch, in a transaction.
        for (int creating = false; creating <= true; ++creating) {
            if (creating) {     // 2nd pass takes place in a transaction
                db->beginTransaction();
                inTransaction = true;
            }

            // Look up the doc in the db, and the remote URL in the doc:
            Record doc = db->getRawDocument(string(kC4InfoStore), slice(kRemoteDBURLsDoc));
            const Dict *remotes = nullptr;
            C4RemoteID remoteID = 0;
            if (doc.exists()) {
                auto body = Value::fromData(doc.body());
                if (body)
                    remotes = body->asDict();
                if (remotes) {
                    auto idObj = remotes->get(remoteAddress);
                    if (idObj)
                        remoteID = C4RemoteID(idObj->asUnsigned());
                }
            }

            if (remoteID > 0) {
                // Found the remote ID!
                return remoteID;
                
            } else if (creating && canCreate) {
                // Update or create the document, adding the identifier:
                remoteID = 1;
                Encoder enc;
                enc.beginDictionary();
                for (Dict::iterator i(remotes); i; ++i) {
                    auto existingID = i.value()->asUnsigned();
                    if (existingID) {
                        enc.writeKey(i.keyString());            // Copy existing entry
                        enc.writeUInt(existingID);
                        remoteID = max(remoteID, 1 + C4RemoteID(existingID));   // make sure new ID is unique
                    }
                }
                enc.writeKey(remoteAddress);                       // Add new entry
                enc.writeUInt(remoteID);
                enc.endDictionary();
                alloc_slice body = enc.extractOutput();

                // Save the doc:
                db->putRawDocument(string(kC4InfoStore), slice(kRemoteDBURLsDoc), nullslice, body);
                db->endTransaction(true);
                inTransaction = false;
                return remoteID;
            }
        }
        if (outError)
            *outError = c4error_make(LiteCoreDomain, kC4ErrorNotFound, {});
        return C4RemoteID(0);
    });
    if (inTransaction)
        c4db_endTransaction(db, false, nullptr);
    return remoteID;
}


C4SliceResult c4doc_getRemoteAncestor(C4Document *doc, C4RemoteID remoteDatabase) C4API {
    return tryCatch<C4SliceResult>(nullptr, [&]{
        return sliceResult(internal(doc)->remoteAncestorRevID(remoteDatabase));
    });
}


bool c4doc_setRemoteAncestor(C4Document *doc, C4RemoteID remoteDatabase, C4Error *outError) C4API {
    return tryCatch<bool>(outError, [&]{
        internal(doc)->setRemoteAncestorRevID(remoteDatabase);
        return true;
    });
}


C4RevisionFlags c4rev_flagsFromDocFlags(C4DocumentFlags docFlags) {
    return Document::currentRevFlagsFromDocFlags(docFlags);
}


// LCOV_EXCL_START
bool c4db_markSynced(C4Database *database, C4String docID, C4SequenceNumber sequence,
                     C4RemoteID remoteID,
                     C4Error *outError) noexcept
{
    bool result = false;
    try {
        if (remoteID == RevTree::kDefaultRemoteID) {
            // Shortcut: can set kSynced flag on the record to mark that the current revision is
            // synced to remote #1. But the call will return false if the sequence no longer
            // matches, i.e this revision is no longer current. Then have to take the slow approach.
            if (database->defaultKeyStore().setDocumentFlag(docID, sequence,
                                                            DocumentFlags::kSynced,
                                                            database->transaction())) {
                return true;
            }
        }

        // Slow path: Load the doc and update the remote-ancestor info in the rev tree:
        unique_ptr<Document> doc(internal(c4doc_get(database, docID, true, outError)));
        if (!doc)
            return false;
        bool found = false;
        do {
            found = (doc->selectedRev.sequence == sequence);
        } while (!found && doc->selectNextRevision());
        if (found) {
            doc->setRemoteAncestorRevID(remoteID);
            result = c4doc_save(doc.get(), 9999, outError);       // don't prune anything
        }
    } catchError(outError)
    return result;
}


#pragma mark - SAVING:


static alloc_slice createDocUUID() {
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
}


// Is this a PutRequest that doesn't require a Record to exist already?
static bool isNewDocPutRequest(C4Database *database, const C4DocPutRequest *rq) {
    if (rq->existingRevision)
        return database->documentFactory().isFirstGenRevID(rq->history[rq->historyCount-1]);
    else
        return rq->historyCount == 0;
}


// Tries to fulfil a PutRequest by creating a new Record. Returns null if one already exists.
static Document* putNewDoc(C4Database *database, const C4DocPutRequest *rq)
{
    DebugAssert(rq->save, "putNewDoc optimization works only if rq->save is true");
    Record record(rq->docID);
    if (!rq->docID.buf)
        record.setKey(createDocUUID());
    Document *idoc = internal(database->documentFactory().newDocumentInstance(record));
    bool ok;
    if (rq->existingRevision)
        ok = (idoc->putExistingRevision(*rq) >= 0);
    else
        ok = idoc->putNewRevision(*rq);
    if (!ok) {
        delete idoc;
        return nullptr;
    }
    return idoc;
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
        if (!docID.buf) {
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
                code = ((idoc->flags & kDocExists) ?kC4ErrorConflict :kC4ErrorNotFound);
            } else if ((idoc->flags & kDocExists) && !(idoc->selectedRev.flags & kDocDeleted)) {
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
    if (rq->docID.buf && !Document::isValidDocID(rq->docID)) {
        c4error_return(LiteCoreDomain, kC4ErrorBadDocID, C4STR("Invalid docID"), outError);
        return nullptr;
    }
    if (rq->existingRevision || rq->historyCount > 0)
        if (!checkParam(rq->docID.buf, "Missing docID", outError))
            return nullptr;
    if (rq->existingRevision) {
        if (!checkParam(rq->historyCount > 0, "No history", outError))
            return nullptr;
    } else {
        if (!checkParam(rq->historyCount <= 1, "Too much history", outError))
            return nullptr;
        if (!checkParam(rq->historyCount > 0 || !(rq->revFlags & kRevDeleted),
                        "Can't create a new already-deleted document", outError))
            return nullptr;
    }

    int commonAncestorIndex = 0;
    C4Document *doc = nullptr;
    try {
        database->validateRevisionBody(rq->body);

        if (rq->save && isNewDocPutRequest(database, rq)) {
            // As an optimization, write the doc assuming there is no prior record in the db:
            doc = putNewDoc(database, rq);
            // If there's already a record, doc will be null, so we'll continue down regular path.
        }
        if (!doc) {
            if (rq->existingRevision) {
                // Insert existing revision:
                doc = c4doc_get(database, rq->docID, false, outError);
                if (!doc)
                    return nullptr;
                commonAncestorIndex = internal(doc)->putExistingRevision(*rq);

            } else {
                // Create new revision:
                C4Slice parentRevID = kC4SliceNull;
                if (rq->historyCount == 1)
                    parentRevID = rq->history[0];
                bool deletion = (rq->revFlags & kRevDeleted) != 0;
                doc = c4doc_getForPut(database, rq->docID, parentRevID, deletion, rq->allowConflict,
                                      outError);
                if (!doc)
                    return nullptr;
                if (!internal(doc)->putNewRevision(*rq))
                    commonAncestorIndex = -1;
            }
        }

        Assert(commonAncestorIndex >= 0, "Unexpected conflict in c4doc_put");
        if (outCommonAncestorIndex)
            *outCommonAncestorIndex = commonAncestorIndex;
        return doc;

    } catchError(outError) {
        c4doc_free(doc);
        return nullptr;
    }
}


C4Document* c4doc_create(C4Database *db,
                         C4String docID,
                         C4Slice revBody,
                         C4RevisionFlags revFlags,
                         C4Error *outError) noexcept
{
    C4DocPutRequest rq = {};
    rq.docID = docID;
    rq.body = revBody;
    rq.revFlags = revFlags;
    rq.save = true;
    return c4doc_put(db, &rq, nullptr, outError);
}


C4Document* c4doc_update(C4Document *doc,
                         C4Slice revBody,
                         C4RevisionFlags revFlags,
                         C4Error *outError) noexcept
{
    auto idoc = internal(doc);
    if (!idoc->mustBeInTransaction(outError))
        return nullptr;
    try {
        idoc->database()->validateRevisionBody(revBody);

        // Why copy the document? Because if we modified it in place it would be too awkward to
        // back out the changes if the save failed. Likewise, the caller may need to be able to
        // back out this entire call if the transaction fails to commit, so having the original
        // doc around helps it.
        unique_ptr<Document> newDoc(idoc->copy());
        C4DocPutRequest rq = {};
        rq.body = revBody;
        rq.revFlags = revFlags;
        rq.allowConflict = true;
        rq.save = true;
        if (newDoc->putNewRevision(rq))
            return newDoc.release();
        c4error_return(LiteCoreDomain, kC4ErrorConflict, C4STR("C4Document is out of date"), outError);
    } catchError(outError)
    return nullptr;
}


bool c4doc_removeRevisionBody(C4Document* doc) noexcept {
    auto idoc = internal(doc);
    return idoc->mustBeInTransaction(NULL) && idoc->removeSelectedRevBody();
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


bool c4doc_resolveConflict(C4Document *doc,
                           C4String winningRevID,
                           C4String losingRevID,
                           C4Slice mergedBody,
                           C4RevisionFlags mergedFlags,
                           C4Error *outError) noexcept
{
    if (!internal(doc)->mustBeInTransaction(outError))
        return false;
    return tryCatch<bool>(outError, [=]{
        internal(doc)->resolveConflict(winningRevID, losingRevID, mergedBody, mergedFlags);
        return true;
    });
}


#pragma mark - FLEECE-SPECIFIC:


using namespace fleece;


FLEncoder c4db_createFleeceEncoder(C4Database* db) noexcept {
    FLEncoder enc = FLEncoder_NewWithOptions(kFLEncodeFleece, 512, true, true);
    FLEncoder_SetSharedKeys(enc, (FLSharedKeys)db->documentKeys());
    return enc;
}


FLEncoder c4db_getSharedFleeceEncoder(C4Database* db) noexcept {
    return db->sharedFLEncoder();
}


C4SliceResult c4db_encodeJSON(C4Database *db, C4Slice jsonData, C4Error *outError) noexcept {
    return tryCatch<C4SliceResult>(outError, [&]{
        Encoder &enc = db->sharedEncoder();
        JSONConverter jc(enc);
        if (!jc.encodeJSON(jsonData)) {
            recordError(FleeceDomain, jc.errorCode(), jc.errorMessage(), outError);
            return C4SliceResult{};
        }
        return sliceResult(enc.extractOutput());
    });
}


C4SliceResult c4doc_bodyAsJSON(C4Document *doc, bool canonical, C4Error *outError) noexcept {
    return tryCatch<C4SliceResult>(outError, [&]{
        return sliceResult(c4Internal::internal(doc)->bodyAsJSON(canonical));
    });
}


FLDictKey c4db_initFLDictKey(C4Database *db, C4Slice string) noexcept {
    return FLDictKey_InitWithSharedKeys({string.buf, string.size},
                                        (FLSharedKeys)db->documentKeys());
}


FLSharedKeys c4db_getFLSharedKeys(C4Database *db) noexcept {
    return (FLSharedKeys)db->documentKeys();
}


bool c4doc_isOldMetaProperty(C4String prop) noexcept {
    return Document::isOldMetaProperty(prop);
}


bool c4doc_hasOldMetaProperties(FLDict doc) noexcept {
    return Document::hasOldMetaProperties((Dict*)doc);
}


bool c4doc_dictIsBlob(FLDict dict, FLSharedKeys sk, C4BlobKey *outKey) C4API {
    assert(outKey);
    return Document::dictIsBlob((const Dict*)dict, *(blobKey*)outKey, (SharedKeys*)sk);
}


bool c4doc_dictContainsBlobs(FLDict dict, FLSharedKeys sk) noexcept {
    bool found = false;
    Document::findBlobReferences((Dict*)dict, (SharedKeys*)sk, [&](const Dict*) {
        found = true;
        return false; // to stop search
    });
    return found;
}


bool c4doc_blobIsCompressible(FLDict blobDict, FLSharedKeys sk) {
    return Document::blobIsCompressible((const Dict*)blobDict, (SharedKeys*)sk);
}


C4SliceResult c4doc_encodeStrippingOldMetaProperties(FLDict doc) noexcept {
    return tryCatch<C4SliceResult>(nullptr, [&]{
        return sliceResult(Document::encodeStrippingOldMetaProperties((const Dict*)doc));
    });
}
