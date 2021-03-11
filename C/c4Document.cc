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


#pragma mark - STATIC UTILITY FUNCTIONS:


    char* C4Document::generateID(char *outDocID, size_t bufferSize) noexcept {
        return Document::generateID(outDocID, bufferSize);
    }

    bool C4Document::equalRevIDs(slice revID1, slice revID2) noexcept {
        return Document::equalRevIDs(revID1, revID2);
    }

    unsigned C4Document::getRevIDGeneration(slice revID) noexcept {
        return Document::getRevIDGeneration(revID);
    }

    C4RevisionFlags C4Document::revisionFlagsFromDocFlags(C4DocumentFlags docFlags) noexcept {
        return Document::currentRevFlagsFromDocFlags(docFlags);
    }

    C4Document* C4Document::containingValue(FLValue value) noexcept {
        return Document::containing(value);
    }

    bool C4Document::containsBlobs(FLDict dict) noexcept {
        bool found = false;
        Document::findBlobReferences(dict, [&](FLDict) {
            found = true;
            return false; // to stop search
        });
        return found;
    }

    bool C4Document::isOldMetaProperty(slice propertyName) noexcept {
        return Document::isOldMetaProperty(propertyName);
    }

    bool C4Document::hasOldMetaProperties(FLDict dict) noexcept {
        return Document::hasOldMetaProperties(dict);
    }

    bool C4Document::isValidDocID(slice docID) noexcept {
        return Document::isValidDocID(docID);
    }

    alloc_slice C4Document::encodeStrippingOldMetaProperties(FLDict properties, FLSharedKeys sk) {
        return Document::encodeStrippingOldMetaProperties(properties, sk);
    }


#pragma mark - REVISIONS:


    bool C4Document::selectCurrentRevision() noexcept {
        return asInternal(this)->selectCurrentRevision();
    }

    bool C4Document::selectRevision(C4Slice revid, bool withBody) {
        return asInternal(this)->selectRevision(revid, withBody);
    }

    bool C4Document::selectParentRevision() noexcept {
        return asInternal(this)->selectParentRevision();
    }

    bool C4Document::selectNextRevision() {
        return asInternal(this)->selectNextRevision();
    }

    bool C4Document::selectNextLeafRevision(bool includeDeleted, bool withBody) {
        bool ok = asInternal(this)->selectNextLeafRevision(includeDeleted);
        if (ok && withBody)
            loadRevisionBody();
        return ok;
    }

    bool C4Document::selectCommonAncestorRevision(slice revID1, slice revID2) {
        return asInternal(this)->selectCommonAncestorRevision(revID1, revID2);
    }


#pragma mark - REVISION INFO:


    bool C4Document::loadRevisionBody() {
        return asInternal(this)->loadSelectedRevBody();
    }

    bool C4Document::hasRevisionBody() noexcept {
        return asInternal(this)->hasRevisionBody();
    }

    slice C4Document::getRevisionBody() noexcept {
        return asInternal(this)->getSelectedRevBody();
    }

    alloc_slice C4Document::bodyAsJSON(bool canonical) {
        return asInternal(this)->bodyAsJSON(canonical);
    }

    FLDict C4Document::getProperties() noexcept {
        return asInternal(this)->getSelectedRevRoot();
    }

    alloc_slice C4Document::getSelectedRevIDGlobalForm() {
        return asInternal(this)->getSelectedRevIDGlobalForm();
    }

    alloc_slice C4Document::getRevisionHistory(unsigned maxHistory,
                                                const C4String backToRevs[],
                                                unsigned backToRevsCount)
    {
        return asInternal(this)->getSelectedRevHistory(maxHistory, backToRevs, backToRevsCount);
    }

    alloc_slice C4Document::getRemoteAncestor(C4RemoteID remote) {
        return asInternal(this)->remoteAncestorRevID(remote);

    }

    void C4Document::setRemoteAncestor(C4RemoteID remote, C4String revid) {
        return asInternal(this)->setRemoteAncestorRevID(remote, revid);
    }


#pragma mark - UPDATING / PURGING / SAVING:


    bool C4Document::removeRevisionBody() noexcept {
        asInternal(this)->mustBeInTransaction();
        return asInternal(this)->removeSelectedRevBody();

    }

    int32_t C4Document::purgeRevision(C4Slice revid) {
        auto doc = asInternal(this);
        doc->mustBeInTransaction();
        if (!doc->loadRevisions())
            error(error::LiteCore, error::Conflict, "C4Document is out of date")._throw();
        return doc->purgeRevision(revid);
    }

    void C4Document::resolveConflict(C4String winningRevID,
                                      C4String losingRevID,
                                      FLDict mergedProperties,
                                      C4RevisionFlags mergedFlags,
                                      bool pruneLosingBranch)
    {
        return asInternal(this)->resolveConflict(winningRevID, losingRevID,
                                                 mergedProperties, mergedFlags, pruneLosingBranch);

    }

    void C4Document::resolveConflict(C4String winningRevID,
                                      C4String losingRevID,
                                      C4Slice mergedBody,
                                      C4RevisionFlags mergedFlags,
                                      bool pruneLosingBranch)
    {
        return asInternal(this)->resolveConflict(winningRevID, losingRevID,
                                                 mergedBody, mergedFlags, pruneLosingBranch);
    }

    Retained<C4Document> C4Document::update(slice revBody, C4RevisionFlags revFlags) {
        return asInternal(this)->update(revBody, revFlags);
    }

    bool C4Document::save(unsigned maxRevTreeDepth) {
        auto idoc = asInternal(this);
        idoc->mustBeInTransaction();
        if (maxRevTreeDepth == 0)
            maxRevTreeDepth = idoc->database()->maxRevTreeDepth();
        return idoc->save(maxRevTreeDepth);
    }
