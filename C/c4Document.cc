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
#include "c4Document.hh"
#include "c4Internal.hh"
#include "c4Database.hh"
#include "c4Document+Fleece.h"
#include "c4Private.h"
#include "c4.hh"

#include "c4BlobStore.hh"
#include "TreeDocument.hh"
#include "VectorDocument.hh"
#include "Document.hh"
#include "Database.hh"
#include "LegacyAttachments.hh"
#include "RevTree.hh"   // only for kDefaultRemoteID
#include "SecureRandomize.hh"
#include "FleeceImpl.hh"

using namespace fleece::impl;
using namespace std;


#pragma mark - C++ API:


C4Document* retain(C4Document *doc) {retain(asInternal(doc)); return doc;}
void release(C4Document *doc)       {release(asInternal(doc));}


namespace c4 {

    // The inheritance is kind of weird...
    //       c4Internal::Document
    //               /  \
    //    RefCounted     C4Document
    //                    \
    //                     c4::DocumentAPI
    //
    // - Document has to inherit from RefCounted first so it can function as a proper
    //   reference-counted object.
    // - It also inherits from C4Document so it has the proper struct members, and to make it simple
    //   to convert a C4Document* to a Document* by casting (C++ does the pointer arithmetic.)
    // - C4Document inherits from c4::DocumentAPI so that C++ clients can call methods on it.
    //
    // The result is that clients call methods on c4::DocumentAPI, which has to be upcast to Document
    // to call into the actual implementations. That's what `asInternal()` is for. Note that this
    // is one of those funky C++ multiple-inheritance casts that actually offsets the pointer,
    // since the `this` of a `DocumentAPI*` is actually 4 bytes into a `Document*`.

    static inline Document* asInternal(DocumentAPI *d) {
        return static_cast<Document*>(d);
    }


#pragma mark - STATIC UTILITY FUNCTIONS:


    char* DocumentAPI::generateID(char *outDocID, size_t bufferSize) noexcept {
        return Document::generateID(outDocID, bufferSize);
    }

    bool DocumentAPI::equalRevIDs(slice revID1, slice revID2) noexcept {
        return Document::equalRevIDs(revID1, revID2);
    }

    unsigned DocumentAPI::getRevIDGeneration(slice revID) noexcept {
        return Document::getRevIDGeneration(revID);
    }

    C4RevisionFlags DocumentAPI::revisionFlagsFromDocFlags(C4DocumentFlags docFlags) noexcept {
        return Document::currentRevFlagsFromDocFlags(docFlags);
    }

    C4Document* DocumentAPI::containingValue(FLValue value) noexcept {
        return Document::containing(value);
    }

    bool DocumentAPI::containsBlobs(FLDict dict) noexcept {
        bool found = false;
        Document::findBlobReferences(dict, [&](FLDict) {
            found = true;
            return false; // to stop search
        });
        return found;
    }

    bool DocumentAPI::isOldMetaProperty(slice propertyName) noexcept {
        return Document::isOldMetaProperty(propertyName);
    }

    bool DocumentAPI::hasOldMetaProperties(FLDict dict) noexcept {
        return Document::hasOldMetaProperties(dict);
    }

    bool DocumentAPI::isValidDocID(slice docID) noexcept {
        return Document::isValidDocID(docID);
    }

    alloc_slice DocumentAPI::encodeStrippingOldMetaProperties(FLDict properties, FLSharedKeys sk) {
        return Document::encodeStrippingOldMetaProperties(properties, sk);
    }


#pragma mark - REVISIONS:


    bool DocumentAPI::selectCurrentRevision() noexcept {
        return asInternal(this)->selectCurrentRevision();
    }

    bool DocumentAPI::selectRevision(C4Slice revID, bool withBody) {
        return asInternal(this)->selectRevision(revID, withBody);
    }

    bool DocumentAPI::selectParentRevision() noexcept {
        return asInternal(this)->selectParentRevision();
    }

    bool DocumentAPI::selectNextRevision() {
        return asInternal(this)->selectNextRevision();
    }

    bool DocumentAPI::selectNextLeafRevision(bool includeDeleted, bool withBody) {
        bool ok = asInternal(this)->selectNextLeafRevision(includeDeleted);
        if (ok && withBody)
            loadRevisionBody();
        return ok;
    }

    bool DocumentAPI::selectCommonAncestorRevision(slice revID1, slice revID2) {
        return asInternal(this)->selectCommonAncestorRevision(revID1, revID2);
    }


#pragma mark - REVISION INFO:


    bool DocumentAPI::loadRevisionBody() {
        return asInternal(this)->loadSelectedRevBody();
    }

    bool DocumentAPI::hasRevisionBody() noexcept {
        return asInternal(this)->hasRevisionBody();
    }

    slice DocumentAPI::getRevisionBody() noexcept {
        return asInternal(this)->getSelectedRevBody();
    }

    alloc_slice DocumentAPI::bodyAsJSON(bool canonical) {
        return asInternal(this)->bodyAsJSON(canonical);
    }

    FLDict DocumentAPI::getProperties() noexcept {
        return asInternal(this)->getSelectedRevRoot();
    }

    alloc_slice DocumentAPI::getSelectedRevIDGlobalForm() {
        return asInternal(this)->getSelectedRevIDGlobalForm();
    }

    alloc_slice DocumentAPI::getRevisionHistory(unsigned maxHistory,
                                                const C4String backToRevs[],
                                                unsigned backToRevsCount)
    {
        return asInternal(this)->getSelectedRevHistory(maxHistory, backToRevs, backToRevsCount);
    }

    alloc_slice DocumentAPI::getRemoteAncestor(C4RemoteID remote) {
        return asInternal(this)->remoteAncestorRevID(remote);

    }

    void DocumentAPI::setRemoteAncestor(C4RemoteID remote, C4String revID) {
        return asInternal(this)->setRemoteAncestorRevID(remote, revID);
    }


#pragma mark - UPDATING / PURGING / SAVING:


    bool DocumentAPI::removeRevisionBody() noexcept {
        asInternal(this)->mustBeInTransaction();
        return asInternal(this)->removeSelectedRevBody();

    }

    int32_t DocumentAPI::purgeRevision(C4Slice revID) {
        auto doc = asInternal(this);
        doc->mustBeInTransaction();
        if (!doc->loadRevisions())
            error(error::LiteCore, error::Conflict, "C4Document is out of date")._throw();
        return doc->purgeRevision(revID);
    }

    void DocumentAPI::resolveConflict(C4String winningRevID,
                                      C4String losingRevID,
                                      FLDict mergedProperties,
                                      C4RevisionFlags mergedFlags,
                                      bool pruneLosingBranch)
    {
        return asInternal(this)->resolveConflict(winningRevID, losingRevID,
                                                 mergedProperties, mergedFlags, pruneLosingBranch);

    }

    void DocumentAPI::resolveConflict(C4String winningRevID,
                                      C4String losingRevID,
                                      C4Slice mergedBody,
                                      C4RevisionFlags mergedFlags,
                                      bool pruneLosingBranch)
    {
        return asInternal(this)->resolveConflict(winningRevID, losingRevID,
                                                 mergedBody, mergedFlags, pruneLosingBranch);
    }

    Retained<C4Document> DocumentAPI::update(slice revBody, C4RevisionFlags flags) {
        return asInternal(this)->update(revBody, flags);
    }

    bool DocumentAPI::save(unsigned maxRevTreeDepth) {
        auto idoc = asInternal(this);
        idoc->mustBeInTransaction();
        if (maxRevTreeDepth == 0)
            maxRevTreeDepth = idoc->database()->maxRevTreeDepth();
        return idoc->save(maxRevTreeDepth);
    }

} // end namespace c4
