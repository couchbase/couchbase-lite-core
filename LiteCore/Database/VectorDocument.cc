//
//  VectorDocument.cc
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 7/18/16.
//  Copyright (c) 2016 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#include "Document.hh"
#include "c4Database.h"
#include "c4Private.h"

#include "CASRevisionStore.hh"
#include "Revision.hh"

#include <algorithm>

namespace c4Internal {

    class VectorDocument : public Document {
    public:

        VectorDocument(VectorDocumentFactory *factory, C4Slice docID)
        :Document(factory->database(), docID),
         _store(factory->revisionStore()),
         _current(_store.get(docID))
        {
            if (!_current) {
                Record doc(docID);
                _current.reset(new Revision(doc));
            }
            init();
        }


        VectorDocument(VectorDocumentFactory *factory, const Record &doc)
        :Document(factory->database(), move(doc)),
         _store(factory->revisionStore()),
         _current(new Revision(doc))
        {
            init();
        }


        ~VectorDocument()
        { }


        void init() {
            _selected = _current;
            currentChanged();
            selectCurrentRevision();
        }


        void currentChanged() {
            docID = _current->docID();
            _revIDBuf = _current->revID();
            revID = _revIDBuf;
            sequence = _current->sequence();
            flags = (C4DocumentFlags)_current->flags();
            if (_current->exists())
                flags = (C4DocumentFlags)(flags | kExists);
        }


        virtual const Record& record() override {
            return _current->record();
        }


        virtual bool exists() override              {return _current->exists();}
        virtual slice type() noexcept override      {return _current->docType();}
        virtual void setType(slice type) noexcept override   {/* Not applicable */}


        virtual void loadRevisions() override {
            if (!_revisionsLoaded) {
                _revisions = _store.allOtherRevisions(docID);
                _revisionsLoaded = true;
            }
        }


        virtual bool revisionsLoaded() const noexcept override   {
            return _revisionsLoaded;
        }


        bool selectRevision(shared_ptr<Revision> rev) {
            _selected = rev;
            _loadedBody = nullslice;
            if (rev) {
                _selectedRevIDBuf = rev->revID();
                selectedRev.revID = _selectedRevIDBuf;
                selectedRev.flags = kRevLeaf;   //FIX: Not true of CAS common-ancestor rev
                if (rev->flags() & Revision::kDeleted)
                    selectedRev.flags |= kRevDeleted;
                if (rev->flags() & Revision::kHasAttachments)
                    selectedRev.flags |= kRevHasAttachments;
                selectedRev.sequence = rev->sequence();
                selectedRev.body = rev->body();
                return true;
            } else {
                clearSelectedRevision();
                return false;
            }
        }


        virtual bool selectRevision(C4Slice revID, bool withBody) override {
            if (revID.buf) {
                shared_ptr<Revision> rev = _store.get(docID, revID,
                                                        (withBody ? kDefaultContent : kMetaOnly));
                if (!selectRevision(rev))
                    return false;
            } else {
                selectRevision(nullptr);
            }
            return true;
        }


        virtual bool selectCurrentRevision() noexcept override {
            if (_current->body().buf) {
                selectRevision(_current);
                return true;
            } else {
                return Document::selectCurrentRevision();
            }
        }


        virtual bool selectParentRevision() noexcept override {
            // TODO: Implement this for the current revision if the CAS ancestor is available
            return false;
        }


        virtual bool selectNextRevision() override {
            return selectNextLeafRevision(false);
        }


        virtual bool selectNextLeafRevision(bool includeDeleted) override {
            loadRevisions();
            if (_revisions.size() == 0)
                return false;
            auto next = _revisions.begin();
            if (_selected && _selected != _current) {
                next = find(next, _revisions.end(), _selected);
                Assert(next != _revisions.end());
                ++next;
            }
            if (next == _revisions.end())
                return false;
            selectRevision(*next);
            return true;
        }


        virtual bool hasRevisionBody() noexcept override {
            return _selected != nullptr;
        }


        virtual bool loadSelectedRevBodyIfAvailable() override {
            if (!_selected)
                return false;
            _store.readBody(*_selected);
            selectedRev.body = _selected->body();
            return true;
        }


        alloc_slice detachSelectedRevBody() override {
            return alloc_slice(_selected->body().copy());    // FIX: Always copies
        }


        virtual int32_t putExistingRevision(const C4DocPutRequest &rq) override {
            VersionVector vers(rq.history[0]);
            Revision::BodyParams bodyParams {rq.body, rq.docType, rq.deletion, rq.hasAttachments};
            shared_ptr<Revision> newRev(new Revision(rq.docID, vers, bodyParams, true));
            auto order = _store.insert(*newRev, _db->transaction());
            if (order == kOlder || order == kSame)
                return 0;
            if (order == kConflicting)
                _current->setConflicted(true);
            selectNewRev(newRev);
            if (newRev->isCurrent())
                _db->saved(this);
            return 1;
        }


        virtual bool putNewRevision(const C4DocPutRequest &rq) override {
            Revision::BodyParams bodyParams {rq.body, rq.docType, rq.deletion, rq.hasAttachments};
            shared_ptr<Revision> newRev = _store.create(rq.docID, _selected->version(), bodyParams,
                                                        _db->transaction());
            if (!newRev)
                return false;
            selectNewRev(newRev);
            if (newRev->isCurrent())
                _db->saved(this);
            return true;
        }

        
        void selectNewRev(shared_ptr<Revision> &newRev) {
            if (newRev->isCurrent()) {
                _current = newRev;
            } else {
                _revisions.insert(_revisions.begin(), newRev);
            }
            currentChanged();
            selectRevision(newRev);
        }


    private:
        CASRevisionStore &_store;                   // The revision storage
        shared_ptr<Revision> _current;              // The doc's current revision (always loaded)
        shared_ptr<Revision> _selected;             // Points to whichever revision is selected
        vector<shared_ptr<Revision> > _revisions;   // Non-current revisions (lazily loaded)
        bool _revisionsLoaded {false};              // Has _revisions been loaded yet?
    };


#pragma mark - FACTORY:


    VectorDocumentFactory::VectorDocumentFactory(Database *db)
    :DocumentFactory(db)
    { }


    CASRevisionStore& VectorDocumentFactory::revisionStore() {
        if (!_revisionStore)
            _revisionStore.reset(new CASRevisionStore(database()->dataFile()));
        return *_revisionStore;
    }


    Document* VectorDocumentFactory::newDocumentInstance(C4Slice docID) {
        return new VectorDocument(this, docID);
    }

    Document* VectorDocumentFactory::newDocumentInstance(const Record &doc) {
        return new VectorDocument(this, doc);
    }


    bool VectorDocumentFactory::readDocMeta(const Record &doc,
                                            C4DocumentFlags *outFlags,
                                            alloc_slice *outRevID,
                                            slice *outDocType)
    {
        Revision rev(doc);
        if (outFlags)
            *outFlags = rev.flags();
        if (outRevID)
            *outRevID = rev.revID();
        if (outDocType)
            *outDocType = rev.docType();
        return true;
    }

}
