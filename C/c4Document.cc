//
// C4Document.cc
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

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "c4Document.hh"
#include "c4BlobStore.hh"
#include "c4Internal.hh"
#include "c4ExceptionUtils.hh"
#include "CollectionImpl.hh"
#include "DatabaseImpl.hh"
#include "DocumentFactory.hh"
#include "LegacyAttachments.hh"
#include "RevID.hh"
#include "RevTree.hh"   // only for kDefaultRemoteID
#include "Base64.hh"
#include "Error.hh"
#include "SecureRandomize.hh"
#include "StringUtil.hh"
#include "Doc.hh"
#include "FleeceImpl.hh"
#include "DeepIterator.hh"

using namespace std;
using namespace fleece;
using namespace litecore;


C4Document::C4Document(C4Collection *collection, alloc_slice docID_)
:_collection(asInternal(collection))
,_docID(move(docID_))
{
    // Quick sanity test of the docID, but no need to scan for valid UTF-8 since we're not inserting.
    if (_docID.size < 1 || _docID.size > kMaxDocIDLength)
        error::_throw(error::BadDocID, "Invalid docID \"%.*s\"", SPLAT(docID_));
#if DEBUG
    // Make sure that C4Document and C4Document_C line up so that the same object can serve as both:
    auto asStruct = (C4Document_C*)this;
    if (&_flags != &(asStruct)->flags || (void*)&_docID != (void*)&(asStruct)->docID) {
        WarnError("FATAL: C4Document struct layout is wrong!! "
                  "this=%p; asStruct=%p; `flags` at %p vs %p, `docID` at %p vs %p",
                  this, asStruct, &_flags, &(asStruct)->flags, &_docID, &(asStruct)->docID);
        /* If this is wrong, using C4Document is completely broken, so best just abort.
           This indicates something wrong with the kludgy padding fields at the start of
           C4Document_C in c4DocumentStruct.h -- they don't match the actual amount of space
           taken up by the vtable and inherited field(s) of C4Document. */
        std::terminate();

        /* So here's what's up with this layout kludge. We have to make the type `C4Document*`
           work equally well in C (as a non-opaque struct with six fields) and in C++ (as a
           class inheriting from RefCounted.) It's the "non-opaque struct" part that's hard, but
           for legacy reasons we have to keep it that way for now.

           So, C4Document extends RefCounted, which contains a vtable pointer and an `int32_t`.
           Therefore we can mimic its layout in C by putting a `void*` and an `int32_t` at the
           start of `C4Document_C`, right? Well, that works with Clang and GCC but not MSVC.
           The latter adds 4 extra bytes of padding before the first field (`flags`). Why?

           It seems to be because there are two different ways to lay out a class with a base
           class. Here the base class is RefCounted, which looks like:
                class RefCounted {
                    _vtable* __vptr;
                    int32_t _refCount;
                };
           The GCC/Clang way to lay out C4Document is to pretend to add the inherited members:
                class C4Document {
                    _vtable* __vptr;
                    int32_t _refCount;
                    C4DocumentFlags _flags;         // (this is a uint32_t basically)
                    ...
                };

           The MSVC way is to pretend to add the base class as a single member:
                class C4Document {
                    RefCounted __baseClass;
                    C4DocumentFlags _flags;
                    ...
                };
           This has different results because sizeof(RefCounted) == 16, not 12! C++ says the
           size of a struct/class has to be a multiple of the alignment of each field, and the
           vtable pointer is 8 bytes.

           I've chosen to work around this by adding `alignas(void*)` to the declaration of the
           `_flags` member, forcing it to be 8-byte aligned on all platforms for consistency.
           Correspondingly, I changed the second `_internal2` padding field of `C4Document_C`
           from `int32_t` to `void*`, so the `flags` field of that struct is also 8-byte aligned.
           --Jens, April 28 2021
         */
    }
#endif
}


C4Document::~C4Document() {
    destructExtraInfo(_extraInfo);
}


C4Collection* C4Document::collection() const                {return _collection;}
C4Database* C4Document::database() const                    {return _collection->getDatabase();}
KeyStore& C4Document::keyStore() const                      {return _collection->keyStore();}


FLDict C4Document::getProperties() const noexcept {
    if (slice body = getRevisionBody(); body)
    return FLValue_AsDict(FLValue_FromData(body, kFLTrusted));
    else
        return nullptr;
}

alloc_slice C4Document::bodyAsJSON(bool canonical) const {
    if (!loadRevisionBody())
        error::_throw(error::NotFound);
    if (FLDict root = getProperties())
        return ((const fleece::impl::Dict*)root)->toJSON(canonical);
    error::_throw(error::CorruptRevisionData);
}


void C4Document::setRevID(revid id) {
    if (id.size > 0)
        _revID = id.expanded();
    else
        _revID = nullslice;
}


bool C4Document::selectCurrentRevision() noexcept {
    // By default just fill in what we know about the current revision:
    if (exists()) {
        _selectedRevID = _revID;
        _selected.revID = _selectedRevID;
        _selected.sequence = _sequence;
        _selected.flags = revisionFlagsFromDocFlags(_flags);
    } else {
        clearSelectedRevision();
    }
    return false;
}


void C4Document::clearSelectedRevision() noexcept {
    _selectedRevID = nullslice;
    _selected.revID = _selectedRevID;
    _selected.flags = (C4RevisionFlags)0;
    _selected.sequence = 0;
}


alloc_slice C4Document::getSelectedRevIDGlobalForm() const {
    // By default just return the same revID
    DebugAssert(_selectedRevID == _selected.revID);
    return _selectedRevID;
}


#pragma mark - SAVING:


alloc_slice C4Document::createDocID() {
    char docID[C4Document::kGeneratedIDLength + 1];
    return alloc_slice(C4Document::generateID(docID, sizeof(docID)));
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
    inError.raise();
}


Retained<C4Document> C4Document::update(slice revBody, C4RevisionFlags revFlags) {
    auto db = asInternal(database());
    db->mustBeInTransaction();
    db->validateRevisionBody(revBody);

    alloc_slice parentRev = _selectedRevID;
    C4DocPutRequest rq = {};
    rq.docID = _docID;
    rq.body = revBody;
    rq.revFlags = revFlags;
    rq.allowConflict = false;
    rq.history = (C4String*)&parentRev;
    rq.historyCount = 1;
    rq.save = true;

    // First the fast path: try to save directly via putNewRevision:
    if (loadRevisions()) {
        C4Error myErr;
        if (checkNewRev(parentRev, revFlags, false, &myErr)
            && putNewRevision(rq, &myErr)) {
            // Fast path succeeded!
            return this;
        } else if (myErr != C4Error{LiteCoreDomain, kC4ErrorConflict}) {
            // Something other than a conflict happened, so give up:
            myErr.raise();
        }
        // on conflict, fall through...
    }

    // MVCC prevented us from writing directly to the document. So instead, read-modify-write:
    C4Error myErr;
    Retained<C4Document> savedDoc = _collection->putDocument(rq, nullptr, &myErr);
    if (!savedDoc) {
        throwIfUnexpected(myErr, nullptr);
        savedDoc = nullptr;
    }
    return savedDoc;
}


bool C4Document::checkNewRev(slice parentRevID,
                             C4RevisionFlags rqFlags,
                             bool allowConflict,
                             C4Error *outError) noexcept
{
    int code = 0;
    if (parentRevID) {
        // Updating an existing revision; make sure it exists and is a leaf:
        if (!exists())
            code = kC4ErrorNotFound;
        else if (!selectRevision(parentRevID, false))
            code = allowConflict ? kC4ErrorNotFound : kC4ErrorConflict;
        else if (!allowConflict && !(_selected.flags & kRevLeaf))
            code = kC4ErrorConflict;
    } else {
        // No parent revision given:
        if (rqFlags & kRevDeleted) {
            // Didn't specify a revision to delete: NotFound or a Conflict, depending
            code = ((_flags & kDocExists) ?kC4ErrorConflict :kC4ErrorNotFound);
        } else if ((_flags & kDocExists) && !(_selected.flags & kRevDeleted)) {
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


#pragma mark - CONFLICTS:


void C4Document::resolveConflict(slice winningRevID,
                                 slice losingRevID,
                                 FLDict mergedProperties,
                                 C4RevisionFlags mergedFlags,
                                 bool pruneLosingBranch)
{
    alloc_slice mergedBody;
    if (mergedProperties) {
        auto enc = database()->sharedFleeceEncoder();
        FLEncoder_WriteValue(enc, (FLValue)mergedProperties);
        FLError flErr;
        mergedBody = FLEncoder_Finish(enc, &flErr);
        if (!mergedBody)
            error::_throw(error::Fleece, flErr);
    }
    return resolveConflict(winningRevID, losingRevID, mergedBody, mergedFlags);
}


#pragma mark - STATIC UTILITY FUNCTIONS:


[[noreturn]] void C4Document::failUnsupported() {
    error::_throw(error::UnsupportedOperation);
}


bool C4Document::isValidDocID(slice docID) noexcept {
    return docID.size >= 1 && docID.size <= kMaxDocIDLength && docID[0] != '_'
        && isValidUTF8(docID) && hasNoControlCharacters(docID);
}


void C4Document::requireValidDocID(slice docID) {
    if (!C4Document::isValidDocID(docID))
        error::_throw(error::BadDocID, "Invalid docID \"%.*s\"", SPLAT(docID));
}


char* C4Document::generateID(char *outDocID, size_t bufferSize) noexcept {
    if (bufferSize < kGeneratedIDLength + 1)
        return nullptr;
    static const char kBase64[65] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
    "0123456789-_";
    uint8_t r[kGeneratedIDLength - 1];
    SecureRandomize({r, sizeof(r)});
    outDocID[0] = '~';
    for (unsigned i = 0; i < sizeof(r); ++i)
    outDocID[i+1] = kBase64[r[i] % 64];
    outDocID[kGeneratedIDLength] = '\0';
    return outDocID;
}


bool C4Document::equalRevIDs(slice rev1, slice rev2) noexcept {
    try {
        if (rev1 == rev2)
            return true;
        revidBuffer buf1, buf2;
        return buf1.tryParse(rev1) && buf2.tryParse(rev2) && buf1.isEquivalentTo(buf2);
    }catchAndWarn()
    return false;
}


unsigned C4Document::getRevIDGeneration(slice revID) noexcept {
    try {
        return revidBuffer(revID).generation();
    }catchAndWarn()
    return 0;
}


C4RevisionFlags C4Document::revisionFlagsFromDocFlags(C4DocumentFlags docFlags) noexcept {
    C4RevisionFlags revFlags = 0;
    if (docFlags & kDocExists) {
        revFlags |= kRevLeaf;
        // For stupid historical reasons C4DocumentFlags and C4RevisionFlags aren't compatible
        if (docFlags & kDocDeleted)
            revFlags |= kRevDeleted;
        if (docFlags & kDocHasAttachments)
            revFlags |= kRevHasAttachments;
        if (docFlags & (C4DocumentFlags)DocumentFlags::kSynced)
            revFlags |= kRevKeepBody;
    }
    return revFlags;
}


C4Document* C4Document::containingValue(FLValue value) noexcept {
    return C4Collection::documentContainingValue(value);
}


bool C4Document::isOldMetaProperty(slice propertyName) noexcept {
    return legacy_attachments::isOldMetaProperty(propertyName);
}


bool C4Document::hasOldMetaProperties(FLDict dict) noexcept {
    return legacy_attachments::hasOldMetaProperties((const fleece::impl::Dict*)dict);
}


alloc_slice C4Document::encodeStrippingOldMetaProperties(FLDict properties, FLSharedKeys sk) {
    return legacy_attachments::encodeStrippingOldMetaProperties((const fleece::impl::Dict*)properties,
                                                                (fleece::impl::SharedKeys*)sk);
}
