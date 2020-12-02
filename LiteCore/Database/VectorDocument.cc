//
// VectorDocument.cc
//
// Copyright © 2020 Couchbase. All rights reserved.
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

#include "VectorDocument.hh"
#include "NuDocument.hh"
#include "VersionVector.hh"
#include "c4Private.h"
#include "StringUtil.hh"

namespace c4Internal {
    using namespace fleece;


    class VectorDocument : public Document {
    public:
        VectorDocument(Database* database, C4Slice docID, ContentOption whichContent)
        :Document(database, docID)
        ,_doc(database->defaultKeyStore(), docID, whichContent)
        {
            _initialize();
        }


        VectorDocument(Database *database, const Record &doc)
        :Document(database, doc.key())
        ,_doc(database->defaultKeyStore(), doc)
        {
            _initialize();
        }


        ~VectorDocument() {
            _doc.owner = nullptr;
        }


        void _initialize() {
            _doc.owner = this;
            _updateDocFields();
            _selectRemote(RemoteID::Local);
        }


        void _updateDocFields() {
            _revIDBuf = _expandRevID(_doc.revID());
            revID = _revIDBuf;

            flags = C4DocumentFlags(_doc.flags());
            if (_doc.exists())
                flags |= kDocExists;
            sequence = _doc.sequence();
        }


        peerID myPeerID() {
            return peerID{database()->myPeerID()};
        }


        alloc_slice _expandRevID(revid rev, peerID myID =kMePeerID) {
            if (rev.isVersion()) {
                // Substitute my real peer ID for the '*':
                return rev.asVersion().asASCII(myID);
            } else {
                return rev.expanded();
            }
        }


        revidBuffer _parseRevID(slice revID) {
            if (revID) {
                if (revidBuffer binaryID(revID); binaryID.isVersion()) {
                    // If it's a version in global form, convert it to local form:
                    if (auto vers = binaryID.asVersion(); vers.author() == myPeerID())
                    binaryID = Version(revID, myPeerID());
                    return binaryID;
                }
            }
            error::_throw(error::BadRevisionID, "Not a version string: '%.*s'", SPLAT(revID));
        }


#pragma mark - SELECTING REVISIONS:


        bool _selectRemote(RemoteID remote) {
            if (auto rev = _doc.remoteRevision(remote); rev) {
                return _selectRemote(remote, *rev);
            } else {
                clearSelectedRevision();
                return false;
            }
        }


        bool _selectRemote(RemoteID remote, Revision &rev) {
            _remoteID = remote;
            _selectedRevIDBuf = _expandRevID(rev.revID);
            selectedRev.revID = _selectedRevIDBuf;
            selectedRev.sequence = _doc.sequence(); // NuDocument doesn't have per-rev sequence

            selectedRev.flags = 0;
            if (remote == RemoteID::Local)  selectedRev.flags |= kRevLeaf;
            if (rev.isDeleted())            selectedRev.flags |= kRevDeleted;
            if (rev.hasAttachments())       selectedRev.flags |= kRevHasAttachments;
            if (rev.isConflicted())         selectedRev.flags |= kRevIsConflict | kRevLeaf;
            return true;
        }


        bool selectRevision(C4Slice revID, bool withBody) override {
            revidBuffer binaryID = _parseRevID(revID);
            RemoteID remote = RemoteID::Local;
            while (auto rev = _doc.remoteRevision(remote)) {
                if (rev->revID == binaryID)
                    return _selectRemote(remote, *rev);
                remote = _doc.nextRemoteID(remote);
            }
            _remoteID = nullopt;
            clearSelectedRevision();
            return false;
        }


        optional<Revision> _selectedRevision() {
            return _remoteID ? _doc.remoteRevision(*_remoteID) : nullopt;
        }


        bool selectCurrentRevision() noexcept override {
            return _selectRemote(RemoteID::Local);
        }


        bool selectNextRevision() override {
            return _remoteID && _selectRemote(_doc.nextRemoteID(*_remoteID));
        }


        bool selectNextLeafRevision(bool includeDeleted) override {
            while (selectNextRevision()) {
                if (selectedRev.flags & kRevLeaf)
                    return true;
            }
            return false;
        }

        
#pragma mark - ACCESSORS:


        slice getSelectedRevBody() noexcept override {
            if (!_remoteID)
                return nullslice;
            else if (*_remoteID != RemoteID::Local)
                error::_throw(error::Unimplemented);    // FIXME: IMPLEMENT
            else if (_doc.contentAvailable() < kCurrentRevOnly)
                return nullslice;
            else
                return _doc.currentRevisionData();
        }


        FLDict getSelectedRevRoot() noexcept override {
            auto rev = _selectedRevision();
            return rev ? rev->properties : nullptr;
        }


        alloc_slice getSelectedRevIDGlobalForm() override {
            auto rev = _selectedRevision();
            return rev ? _expandRevID(rev->revID, myPeerID()) : nullslice;
        }


        alloc_slice getSelectedRevHistory(unsigned maxRevs) override {
            if (auto rev = _selectedRevision(); rev) {
                VersionVector vers;
                vers.readBinary(rev->revID);
                if (vers.count() > maxRevs)
                    vers.limitCount(maxRevs);
                return vers.asASCII(myPeerID());
            } else {
                return nullslice;
            }
        }


        alloc_slice remoteAncestorRevID(C4RemoteID remote) override {
            if (auto rev = _doc.remoteRevision(RemoteID(remote)))
                return rev->revID.expanded();
            return nullptr;
        }


        void setRemoteAncestorRevID(C4RemoteID) override {
            error::_throw(error::Unimplemented);    // FIXME: IMPLEMENT
        }


#pragma mark - EXISTENCE / LOADING:


        bool exists() override {
            return _doc.exists();
        }


        bool hasRevisionBody() noexcept override {
            return _doc.exists() && _remoteID;
        }


        bool loadSelectedRevBody() override {
            if (!_remoteID)
                return false;
            auto which = (*_remoteID == RemoteID::Local) ? kCurrentRevOnly : kEntireBody;
            return _doc.loadData(which);
        }


#pragma mark - SAVING:


        VersionVector _currentVersionVector() {
            auto revID = _doc.revID();
            return revID ? revID.asVersionVector() : VersionVector();
        }

        
        fleece::Doc _newProperties(const C4DocPutRequest &rq, C4Error *outError) {
            alloc_slice body;
            Doc fldoc;
            if (rq.deltaCB == nullptr) {
                body = (rq.allocedBody.buf)? rq.allocedBody : alloc_slice(rq.body);
                if (!body)
                    body = alloc_slice{(FLDict)Dict::emptyDict(), 2};
            } else {
                // Apply a delta via a callback:
                slice delta = (rq.allocedBody.buf)? slice(rq.allocedBody) : slice(rq.body);
                if (!rq.deltaSourceRevID.buf || !selectRevision(rq.deltaSourceRevID, true)) {
                    recordError(LiteCoreDomain, kC4ErrorDeltaBaseUnknown,
                                "Unknown source revision ID for delta", outError);
                    return nullptr;
                } else if (!getSelectedRevBody()) {
                    recordError(LiteCoreDomain, kC4ErrorDeltaBaseUnknown,
                                "Missing source revision body for delta", outError);
                    return nullptr;
                } else {
                    body = rq.deltaCB(rq.deltaCBContext, this, delta, outError);
                }
            }

            // Now validate that the body is OK:
            database()->validateRevisionBody(body);
            fldoc = Doc(body, kFLUntrusted, (FLSharedKeys)database()->documentKeys());
            Assert(fldoc.asDict());     // validateRevisionBody should have preflighted this
            return fldoc;
        }


        bool putNewRevision(const C4DocPutRequest &rq) override {
            if (rq.remoteDBID != 0)
                error::_throw(error::InvalidParameter, "remoteDBID cannot be used when existing=false");
            if (rq.historyCount > 0 && rq.history[0].buf
                    && _parseRevID(rq.history[0]) != _doc.revID())
                error::_throw(error::Conflict);

            // Update the flags:
            Revision newRev;
            newRev.flags = DocumentFlags(docFlagsFromCurrentRevFlags(rq.revFlags) & ~kDocExists);

            // Update the version vector:
            auto newVers = _currentVersionVector();
            newVers.incrementGen(kMePeerID);
            alloc_slice newRevID = newVers.asBinary();
            newRev.revID = revid(newRevID);

            // Update the local body:
            C4Error err;
            Doc fldoc = _newProperties(rq, &err);
            if (!fldoc)
                error::_throw((error::Domain)err.domain, err.code); //FIX: Ick.
            newRev.properties = fldoc.asDict();

            // Store in NuDocument, and update C4Document properties:
            _doc.setCurrentRevision(newRev);
            _selectRemote(RemoteID::Local);
            return _saveNewRev(rq, newRev);
        }


        int32_t putExistingRevision(const C4DocPutRequest &rq, C4Error *outError) override {
            auto remote = RemoteID(rq.remoteDBID);
            Revision newRev;
            newRev.flags = DocumentFlags(docFlagsFromCurrentRevFlags(rq.revFlags) & ~kDocExists);
            Doc fldoc = _newProperties(rq, outError);
            if (!fldoc)
                return -1;
            newRev.properties = fldoc.asDict();
            auto newVers = VersionVector::fromASCII(rq.history[0], myPeerID());
            alloc_slice newVersBinary = newVers.asBinary();
            newRev.revID = revid(newVersBinary);

            int commonAncestor = 1;
            auto order = kNewer;
            if (_doc.exists()) {
                // See whether to update the local revision:
                auto localVers = _currentVersionVector();
                order = newVers.compareTo(localVers);
            }
            switch (order) {
                case kSame:
                case kOlder:
                    // I already have this revision, don't update local
                    commonAncestor = 0;
                    break;
                case kNewer:
                    // It's newer, so update local to this revision:
                    _doc.setCurrentRevision(newRev);
                    break;
                case kConflicting:
                    // Conflict, so update only the remote (if any):
                    if (remote == RemoteID::Local) {
                        c4error_return(LiteCoreDomain, kC4ErrorConflict, nullslice, outError);
                        return -1;
                    }
                    newRev.flags = newRev.flags | DocumentFlags::kConflicted;
                    break;
            }
            
            if (remote != RemoteID::Local) {
                // If this is a revision from a remote, update it in the doc:
                _doc.setRemoteRevision(remote, newRev);
            }

            // Update C4Document.selectedRev:
            _selectRemote(remote);

            // Save to DB, if requested:
            if (!_saveNewRev(rq, newRev))
                return -1;

            return commonAncestor;
        }


        bool _saveNewRev(const C4DocPutRequest &rq, const Revision &newRev) {
            if (rq.save) {
                if (!save())
                    return false;
                if (_db->dataFile()->willLog(LogLevel::Verbose)) {
                    alloc_slice revID = newRev.revID.expanded();
                    _db->dataFile()->_logVerbose( "%-s '%.*s' rev #%.*s as seq %" PRIu64,
                                                 ((rq.revFlags & kRevDeleted) ? "Deleted" : "Saved"),
                                                 SPLAT(rq.docID), SPLAT(revID), sequence);
                }
            }
            return true;
        }


        bool save(unsigned maxRevTreeDepth =0) override {
            requireValidDocID();
            switch (_doc.save(database()->transaction())) {
                case NuDocument::kNoSave:
                    return true;
                case NuDocument::kNoNewSequence:
                    _updateDocFields();  // flags may have changed
                    return true;
                case NuDocument::kConflict:
                    return false;
                case NuDocument::kNewSequence:
                    _updateDocFields();
                    _selectRemote(RemoteID::Local);
                    if (_doc.sequence() > sequence)
                        sequence = selectedRev.sequence = _doc.sequence();
                    database()->documentSaved(this);
                    return true;
            }
        }


    private:
        NuDocument          _doc;
        optional<RemoteID>  _remoteID = RemoteID::Local;    // Identifies selected revision
    };


#pragma mark - FACTORY:


    Retained<Document> VectorDocumentFactory::newDocumentInstance(C4Slice docID) {
        return new VectorDocument(database(), docID, kEntireBody);
    }


    Retained<Document> VectorDocumentFactory::newDocumentInstance(const Record &record) {
        return new VectorDocument(database(), record);
    }


    Retained<Document> VectorDocumentFactory::newLeafDocumentInstance(C4Slice docID,
                                                                      C4Slice revID,
                                                                      bool withBody)
    {
        ContentOption opt = kMetaOnly;
        if (revID.buf)
            opt = kEntireBody;
        else if (withBody)
            opt = kCurrentRevOnly;
        Retained<VectorDocument> doc = new VectorDocument(database(), docID, opt);
        if (revID.buf)
            doc->selectRevision(revID, true);
        return doc;
    }


    Document* VectorDocumentFactory::documentContaining(FLValue value) {
        if (auto nuDoc = NuDocument::containing(value); nuDoc)
            return (VectorDocument*)nuDoc->owner;
        else
            return nullptr;
    }


    vector<alloc_slice> VectorDocumentFactory::findAncestors(const vector<slice> &docIDs,
                                                             const vector<slice> &revIDs,
                                                             unsigned maxAncestors,
                                                             bool mustHaveBodies,
                                                             C4RemoteID remoteDBID)
    {
        error::_throw(error::Unimplemented);    // FIXME: IMPLEMENT
    }


}
