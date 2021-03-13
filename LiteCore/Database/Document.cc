//
// Document.cc
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

#include "Document.hh"
#include "TreeDocument.hh"
#include "VectorDocument.hh"
#include "c4BlobStore.hh"
#include "Base64.hh"
#include "LegacyAttachments.hh"
#include "StringUtil.hh"
#include "Doc.hh"
#include "RevID.hh"
#include "DeepIterator.hh"
#include "SecureRandomize.hh"

using namespace std;
using namespace fleece;
using namespace fleece::impl;

namespace c4Internal {


    bool Document::equalRevIDs(slice rev1, slice rev2) noexcept {
        try {
            if (slice(rev1) == slice(rev2))
                return true;
            revidBuffer buf1, buf2;
            return buf1.tryParse(rev1) && buf2.tryParse(rev2) && buf1.isEquivalentTo(buf2);
        }catchExceptions()
        return false;
    }


    unsigned Document::getRevIDGeneration(slice revID) noexcept {
        try {
            return revidBuffer(revID).generation();
        }catchExceptions()
        return 0;
    }


    void Document::setRevID(revid id) {
        if (id.size > 0)
            _revIDBuf = id.expanded();
        else
            _revIDBuf = nullslice;
        revID = _revIDBuf;
    }

    char* Document::generateID(char *docID, size_t bufferSize) noexcept {
        if (bufferSize < kC4GeneratedIDLength + 1)
            return nullptr;
        static const char kBase64[65] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
        "0123456789-_";
        uint8_t r[kC4GeneratedIDLength - 1];
        SecureRandomize({r, sizeof(r)});
        docID[0] = '~';
        for (unsigned i = 0; i < sizeof(r); ++i)
        docID[i+1] = kBase64[r[i] % 64];
        docID[kC4GeneratedIDLength] = '\0';
        return docID;
    }


    Document* Document::containing(FLValue value) noexcept {
        auto doc = VectorDocumentFactory::documentContaining(value);
        if (!doc)
            doc = TreeDocumentFactory::documentContaining(value);
        return doc;
    }

    
    bool Document::isOldMetaProperty(slice propertyName) noexcept {
        return legacy_attachments::isOldMetaProperty(propertyName);
    }


    bool Document::hasOldMetaProperties(FLDict doc) noexcept {
        return legacy_attachments::hasOldMetaProperties((Dict*)doc);
    }


    alloc_slice Document::encodeStrippingOldMetaProperties(FLDict properties, FLSharedKeys sk) {
        return legacy_attachments::encodeStrippingOldMetaProperties((const Dict*)properties,
                                                                    (SharedKeys*)sk);
    }


    void Document::resolveConflict(C4String winningRevID,
                                 C4String losingRevID,
                                 FLDict mergedProperties,
                                 C4RevisionFlags mergedFlags,
                                 bool pruneLosingBranch)
    {
        alloc_slice mergedBody;
        if (mergedProperties) {
            auto enc = database()->sharedFLEncoder();
            FLEncoder_WriteValue(enc, (FLValue)mergedProperties);
            FLError flErr;
            mergedBody = FLEncoder_Finish(enc, &flErr);
            if (!mergedBody)
                error::_throw(error::Fleece, flErr);
        }
        return resolveConflict(winningRevID, losingRevID, mergedBody, mergedFlags);
    }


    alloc_slice Document::bodyAsJSON(bool canonical) {
        if (!loadSelectedRevBody())
            error::_throw(error::NotFound);
        if (FLDict root = getSelectedRevRoot())
            return ((const Dict*)root)->toJSON(canonical);
        error::_throw(error::CorruptRevisionData);
    }


    // Finds blob references in a Fleece Dict, recursively.
    bool Document::findBlobReferences(FLDict dict, const FindBlobCallback &callback) {
        if (!dict)
            return true;
        for (DeepIterator i((const Dict*)dict); i; ++i) {
            auto d = FLDict(i.value()->asDict());
            if (d && C4Blob::isBlob(d)) {
                if (!callback(d))
                    return false;
                i.skipChildren();
            }
        }
        return true;
    }


    bool Document::isValidDocID(slice docID) noexcept {
        return docID.size >= 1 && docID.size <= 240 && docID[0] != '_'
            && isValidUTF8(docID) && hasNoControlCharacters(docID);
    }

    void Document::requireValidDocID() {
        if (!isValidDocID(docID))
            error::_throw(error::BadDocID, "Invalid docID \"%.*s\"", SPLAT(docID));
    }


#pragma mark - SAVING:


    static alloc_slice createDocUUID() {
        char docID[kC4GeneratedIDLength + 1];
        return alloc_slice(c4doc_generateID(docID, sizeof(docID)));
    }


    // Is this a PutRequest that doesn't require a Record to exist already?
    static bool isNewDocPutRequest(DatabaseImpl *database, const C4DocPutRequest &rq) {
        if (rq.deltaCB)
            return false;
        else if (rq.existingRevision)
            return database->documentFactory().isFirstGenRevID(rq.history[rq.historyCount-1]);
        else
            return rq.historyCount == 0;
    }


    // Tries to fulfil a PutRequest by creating a new Record. Returns null if one already exists.
    static pair<Retained<Document>,int> putNewDoc(DatabaseImpl *database,
                                                  const C4DocPutRequest &rq)
    {
        DebugAssert(rq.save, "putNewDoc optimization works only if rq.save is true");
        Record record(rq.docID);
        if (!rq.docID.buf)
            record.setKey(createDocUUID());
        Retained<Document> idoc = database->documentFactory().newDocumentInstance(record);
        int commonAncestorIndex;
        if (rq.existingRevision)
            commonAncestorIndex = idoc->putExistingRevision(rq, nullptr);
        else
            commonAncestorIndex = idoc->putNewRevision(rq, nullptr) ? 0 : -1;
        if (commonAncestorIndex < 0)
            idoc = nullptr;
        return {idoc, commonAncestorIndex};
    }


    static bool checkNewRev(Document *idoc,
                            slice parentRevID,
                            C4RevisionFlags flags,
                            bool allowConflict,
                            C4Error *outError) noexcept
    {
        int code = 0;

        if (!idoc)
            code = kC4ErrorNotFound;
        else if (parentRevID) {
            // Updating an existing revision; make sure it exists and is a leaf:
            if (!idoc->exists())
                code = kC4ErrorNotFound;
            else if (!idoc->selectRevision(parentRevID, false))
                code = allowConflict ? kC4ErrorNotFound : kC4ErrorConflict;
            else if (!allowConflict && !(idoc->selectedRev.flags & kRevLeaf))
                code = kC4ErrorConflict;
        } else {
            // No parent revision given:
            if (flags & kRevDeleted) {
                // Didn't specify a revision to delete: NotFound or a Conflict, depending
                code = ((idoc->flags & kDocExists) ?kC4ErrorConflict :kC4ErrorNotFound);
            } else if ((idoc->flags & kDocExists) && !(idoc->selectedRev.flags & kDocDeleted)) {
                // If doc exists, current rev must be a deletion or there will be a conflict:
                code = kC4ErrorConflict;
            }
        }

        if (code) {
            c4error_return(LiteCoreDomain, code, nullslice, outError);
            return false;
        }
        return true;
    }


    // Errors other than NotFound, Conflict and delta failures
    // should be thrown as exceptions, in the C++ API.
    static void throwIfUnexpected(const C4Error &inError, C4Error *outError) {
        if (outError)
            *outError = inError;
        if (inError.domain == LiteCoreDomain) {
            switch (inError.code) {
                case kC4ErrorNotFound:
                case kC4ErrorConflict:
                case kC4ErrorDeltaBaseUnknown:
                case kC4ErrorCorruptDelta:
                    return; // don't throw these errors
            }
        }
        C4Error::raise(inError);
    }


    Retained<Document> Document::update(slice revBody, C4RevisionFlags revFlags) {
        mustBeInTransaction();
        database()->validateRevisionBody(revBody);

        alloc_slice parentRev = selectedRev.revID;
        C4DocPutRequest rq = {};
        rq.docID = docID;
        rq.body = revBody;
        rq.revFlags = revFlags;
        rq.allowConflict = false;
        rq.history = (C4String*)&parentRev;
        rq.historyCount = 1;
        rq.save = true;

        // First the fast path: try to save directly via putNewRevision:
        if (loadRevisions()) {
            C4Error myErr;
            if (checkNewRev(this, parentRev, revFlags, false, &myErr)
                    && putNewRevision(rq, &myErr)) {
                // Fast path succeeded!
                return this;
            } else if (myErr != C4Error{LiteCoreDomain, kC4ErrorConflict}) {
                // Something other than a conflict happened, so give up:
                C4Error::raise(myErr);
            }
            // on conflict, fall through...
        }

        // MVCC prevented us from writing directly to the document. So instead, read-modify-write:
        C4Error myErr;
        Retained<Document> savedDoc = database()->putDocument(rq, nullptr, &myErr);
        if (!savedDoc) {
            throwIfUnexpected(myErr, nullptr);
            savedDoc = nullptr;
        }
        return savedDoc;
    }


    Retained<Document> DatabaseImpl::putDocument(const C4DocPutRequest &rq,
                                               size_t *outCommonAncestorIndex,
                                               C4Error *outError)
    {
        mustBeInTransaction();
        if (rq.docID.buf && !Document::isValidDocID(rq.docID))
            error::_throw(error::BadDocID);
        if (rq.existingRevision || rq.historyCount > 0)
            AssertParam(rq.docID.buf, "Missing docID");
        if (rq.existingRevision) {
            AssertParam(rq.historyCount > 0, "No history");
        } else {
            AssertParam(rq.historyCount <= 1, "Too much history");
            AssertParam(rq.historyCount > 0 || !(rq.revFlags & kRevDeleted),
                        "Can't create a new already-deleted document");
            AssertParam(rq.remoteDBID == 0, "remoteDBID cannot be used when existingRevision=false");
        }

        int commonAncestorIndex = 0;
        Retained<Document> doc;
        if (rq.save && isNewDocPutRequest(this, rq)) {
            // As an optimization, write the doc assuming there is no prior record in the db:
            tie(doc, commonAncestorIndex) = putNewDoc(this, rq);
            // If there's already a record, doc will be null, so we'll continue down regular path.
        }
        if (!doc) {
            if (rq.existingRevision) {
                // Insert existing revision:
                doc = getDocument(rq.docID, false, kDocGetAll);
                C4Error err;
                commonAncestorIndex = doc->putExistingRevision(rq, &err);
                if (commonAncestorIndex < 0) {
                    throwIfUnexpected(err, outError);
                    doc = nullptr;
                    commonAncestorIndex = 0;
                }
            } else {
                // Create new revision:
                slice docID = rq.docID;
                alloc_slice newDocID;
                if (!docID)
                    docID = newDocID = createDocUUID();

                slice parentRevID;
                if (rq.historyCount > 0)
                    parentRevID = rq.history[0];

                doc = getDocument(docID, false, kDocGetAll);
                C4Error err;
                if (!checkNewRev(doc, parentRevID, rq.revFlags, rq.allowConflict, &err)
                        || !doc->putNewRevision(rq, &err)) {
                    throwIfUnexpected(err, outError);
                    doc = nullptr;
                }
                commonAncestorIndex = 0;
            }
        }

        Assert(commonAncestorIndex >= 0, "Unexpected conflict in c4doc_put");
        if (outCommonAncestorIndex)
            *outCommonAncestorIndex = commonAncestorIndex;
        return doc;
    }

} // end namespace c4Internal
