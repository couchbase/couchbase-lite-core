//
// NuDocumentFactory.cc
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

#include "NuDocumentFactory.hh"
#include "NuDocument.hh"
#include "VersionVector.hh"
#include "c4Private.h"
#include "StringUtil.hh"

namespace c4Internal {
    using namespace fleece;


    class NuDocumentAdapter : public Document {
    public:
        NuDocumentAdapter(Database* database, C4Slice docID, ContentOption whichContent)
        :Document(database, docID)
        ,_versionedDoc(database->defaultKeyStore(), docID, whichContent)
        {
            initialize();
        }

        NuDocumentAdapter(Database *database, const Record &doc)
        :Document(database, doc.key())
        ,_versionedDoc(database->defaultKeyStore(), doc)
        {
            initialize();
        }

        void initialize() {
            updateDocFields();
            _selectRemote(RemoteID::Local);
        }

        void updateDocFields() {
            setRevID(_versionedDoc.revID());
            flags = C4DocumentFlags(_versionedDoc.flags());
            if (_versionedDoc.exists())
                flags |= kDocExists;
            sequence = _versionedDoc.sequence();
        }

        bool _selectRemote(RemoteID remote) {
            if (auto rev = _versionedDoc.remoteRevision(remote); rev) {
                return _selectRemote(remote, *rev);
            } else {
                clearSelectedRevision();
                return false;
            }
        }

        bool _selectRemote(RemoteID remote, Revision &rev) {
            _remoteID = remote;
            _selectedRevIDBuf = rev.revID.expanded();
            selectedRev.revID = _selectedRevIDBuf;
            selectedRev.sequence = _versionedDoc.sequence(); // NuDocument doesn't have per-rev sequence

            selectedRev.flags = 0;
            if (remote == RemoteID::Local)
                selectedRev.flags |= kRevLeaf;
            if (rev.isDeleted())
                selectedRev.flags |= kRevDeleted;
            if (rev.hasAttachments())
                selectedRev.flags |= kRevHasAttachments;
            if (rev.isConflicted())
                selectedRev.flags |= kRevIsConflict | kRevLeaf;
            return true;
        }

        virtual bool selectRevision(C4Slice revID, bool withBody) override {
            revidBuffer binaryID(revID);
            RemoteID remote = RemoteID::Local;
            while (auto rev = _versionedDoc.remoteRevision(remote)) {
                if (rev->revID == binaryID)
                    return _selectRemote(remote, *rev);
                remote = _versionedDoc.nextRemoteID(remote);
            }
            _remoteID = nullopt;
            clearSelectedRevision();
            return false;
        }

        virtual bool selectCurrentRevision() noexcept override {
            return _selectRemote(RemoteID::Local);
        }

        virtual bool selectNextRevision() override {
            return _remoteID && _selectRemote(_versionedDoc.nextRemoteID(*_remoteID));
        }

        virtual bool selectParentRevision() noexcept override {
            error::_throw(error::Unimplemented);    // FIXME: IMPLEMENT
        }

        virtual bool selectNextLeafRevision(bool includeDeleted) override {
            while (selectNextRevision()) {
                if (selectedRev.flags & kRevLeaf)
                    return true;
            }
            return false;
        }

        virtual bool exists() override {
            return _versionedDoc.exists();
        }

        virtual alloc_slice remoteAncestorRevID(C4RemoteID remote) override {
            if (auto rev = _versionedDoc.remoteRevision(RemoteID(remote)))
                return rev->revID.expanded();
            return nullptr;
        }

        virtual void setRemoteAncestorRevID(C4RemoteID) override {
            error::_throw(error::Unimplemented);    // FIXME: IMPLEMENT
        }

        virtual bool hasRevisionBody() noexcept override {
            return _versionedDoc.exists() && _remoteID;
        }

        virtual bool loadSelectedRevBody() override {
            if (!_remoteID)
                return false;
            auto which = (*_remoteID == RemoteID::Local) ? kCurrentRevOnly : kEntireBody;
            return _versionedDoc.loadData(which);
        }

        virtual slice getSelectedRevBody() noexcept override {
            if (!_remoteID)
                return nullslice;
            else if (*_remoteID != RemoteID::Local)
                error::_throw(error::Unimplemented);    // FIXME: IMPLEMENT
            else if (_versionedDoc.contentAvailable() < kCurrentRevOnly)
                return nullslice;
            else
                return _versionedDoc.currentRevisionData();
        }

        virtual FLDict getSelectedRevRoot() noexcept override {
            if (_remoteID)
                if (auto rev = _versionedDoc.remoteRevision(*_remoteID); rev)
                    return rev->properties;
            return nullptr;
        }

        virtual alloc_slice getSelectedRevHistory(unsigned maxRevs) override {
            if (!_remoteID)
                return nullslice;
            if (auto rev = _versionedDoc.remoteRevision(*_remoteID); rev) {
                VersionVector vers;
                vers.readBinary(rev->revID);
                if (vers.count() > maxRevs)
                    vers.limitCount(maxRevs);
                return vers.asASCII();
            } else {
                return nullslice;
            }
        }


#pragma mark - SAVING:


        VersionVector currentVersionVector() {
            auto revID = _versionedDoc.revID();
            return revID ? revID.asVersionVector() : VersionVector();
        }

        
        fleece::Doc newProperties(const C4DocPutRequest &rq) {
            alloc_slice body = (rq.allocedBody.buf)? rq.allocedBody : alloc_slice(rq.body);
            if (!body)
                return nil;
            database()->validateRevisionBody(body);
            Doc fldoc = Doc(body, kFLUntrusted, (FLSharedKeys)database()->documentKeys());
            Assert(fldoc.asDict());     // validateRevisionBody should have preflighted this
            return fldoc;
        }


        virtual bool putNewRevision(const C4DocPutRequest &rq) override {
            if (rq.remoteDBID != 0)
                error::_throw(error::InvalidParameter, "remoteDBID cannot be used when existing=false");
            if (rq.historyCount > 0 && rq.history[0] != _versionedDoc.revID().expanded())
                error::_throw(error::Conflict);
            if (rq.deltaCB != nullptr)
                error::_throw(error::Unimplemented);    // FIXME: IMPLEMENT

            // Update the flags:
            Revision newRev;
            newRev.flags = DocumentFlags(docFlagsFromCurrentRevFlags(rq.revFlags) & ~kDocExists);

            // Update the version vector:
            auto newVers = currentVersionVector();
            newVers.incrementGen(kMePeerID);
            alloc_slice newRevID = newVers.asBinary();
            newRev.revID = revid(newRevID);

            // Update the local body:
            Doc fldoc = newProperties(rq);
            newRev.properties = fldoc.asDict();

            // Store in NuDocument, and update C4Document properties:
            _versionedDoc.setCurrentRevision(newRev);
            _selectRemote(RemoteID::Local);
            return saveNewRev(rq, newRev);
        }


        virtual int32_t putExistingRevision(const C4DocPutRequest &rq, C4Error *outError) override {
            auto remote = RemoteID(rq.remoteDBID);
            Revision newRev;
            newRev.flags = DocumentFlags(docFlagsFromCurrentRevFlags(rq.revFlags) & ~kDocExists);
            Doc fldoc = newProperties(rq);
            newRev.properties = fldoc.asDict();
            auto newVers = VersionVector::fromASCII(rq.history[0]);
            alloc_slice newVersBinary = newVers.asBinary();
            newRev.revID = revid(newVersBinary);

            int commonAncestor = 1;
            auto order = kNewer;
            if (_versionedDoc.exists()) {
                // See whether to update the local revision:
                auto localVers = currentVersionVector();
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
                    _versionedDoc.setCurrentRevision(newRev);
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
                _versionedDoc.setRemoteRevision(remote, newRev);
            }

            // Update C4Document.selectedRev:
            _selectRemote(remote);

            // Save to DB, if requested:
            if (!saveNewRev(rq, newRev))
                return -1;

            return commonAncestor;
        }


        bool saveNewRev(const C4DocPutRequest &rq, const Revision &newRev) {
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


        virtual bool save(unsigned maxRevTreeDepth =0) override {
            requireValidDocID();
            switch (_versionedDoc.save(database()->transaction())) {
                case NuDocument::kNoSave:
                    return true;
                case NuDocument::kNoNewSequence:
                    updateDocFields();  // flags may have changed
                    return true;
                case NuDocument::kConflict:
                    return false;
                case NuDocument::kNewSequence:
                    updateDocFields();
                    _selectRemote(RemoteID::Local);
                    if (_versionedDoc.sequence() > sequence)
                        sequence = selectedRev.sequence = _versionedDoc.sequence();
                    database()->documentSaved(this);
                    return true;
            }
        }

    private:
        NuDocument          _versionedDoc;
        optional<RemoteID>  _remoteID = RemoteID::Local;
    };


#pragma mark - FACTORY:


    Retained<Document> NuDocumentFactory::newDocumentInstance(C4Slice docID) {
        return new NuDocumentAdapter(database(), docID, kEntireBody);
    }


    Retained<Document> NuDocumentFactory::newDocumentInstance(const Record &record) {
        return new NuDocumentAdapter(database(), record);
    }


    Retained<Document> NuDocumentFactory::newLeafDocumentInstance(C4Slice docID,
                                                                  C4Slice revID,
                                                                  bool withBody)
    {
        ContentOption opt = kMetaOnly;
        if (revID.buf)
            opt = kEntireBody;
        else if (withBody)
            opt = kCurrentRevOnly;
        Retained<NuDocumentAdapter> doc = new NuDocumentAdapter(database(), docID, opt);
        if (revID.buf)
            doc->selectRevision(revID, true);
        return doc;
    }


    slice NuDocumentFactory::fleeceAccessor(slice docBody) const {
        return docBody;
    }


    alloc_slice NuDocumentFactory::revIDFromVersion(slice version) const {
        return revid(version).expanded();
    }


    vector<alloc_slice> NuDocumentFactory::findAncestors(const vector<slice> &docIDs,
                                                         const vector<slice> &revIDs,
                                                         unsigned maxAncestors,
                                                         bool mustHaveBodies,
                                                         C4RemoteID remoteDBID)
    {
        error::_throw(error::Unimplemented);    // FIXME: IMPLEMENT
    }


}
