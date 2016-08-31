//
//  c4Document2.cc
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

#include "c4Internal.hh"
#include "c4Document.h"
#include "c4Database.h"
#include "c4Private.h"

#include "c4DatabaseInternal.hh"
#include "C4DocInternal.hh"
#include "CASRevisionStore.hh"
#include "Revision.hh"


namespace c4Internal {

    class C4DocumentV2 : public C4DocumentInternal {
    public:

        C4DocumentV2(c4DatabaseV2* database, C4Slice docID)
        :C4DocumentInternal(database, docID),
         _store(database->revisionStore()),
         _current(_store.get(docID))
        {
            if (!_current) {
                Document doc(docID);
                _current.reset(new Revision(doc));
            }
            init();
        }


        C4DocumentV2(c4DatabaseV2 *database, const Document &doc)
        :C4DocumentInternal(database, move(doc)),
         _store(database->revisionStore()),
         _current(new Revision(doc))
        {
            init();
        }


        ~C4DocumentV2();


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


        virtual const Document& document() override {
            return _current->document();
        }


        virtual bool exists() override              {return _current->exists();}
        virtual slice type() override               {return _current->docType();}
        virtual void setType(slice type) override   {/* Not applicable */}


        virtual void loadRevisions() override {
            if (!_revisionsLoaded) {
                _revisions = _store.allOtherRevisions(docID);
                _revisionsLoaded = true;
            }
        }


        virtual bool revisionsLoaded() const override   {
            return _revisionsLoaded;
        }


        bool selectRevision(shared_ptr<Revision> rev) {
            _selected = rev;
            _loadedBody = slice::null;
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
                selectRevision(NULL);
            }
            return true;
        }


        virtual bool selectCurrentRevision() override {
            if (_current->body().buf) {
                selectRevision(_current);
                return true;
            } else {
                return C4DocumentInternal::selectCurrentRevision();
            }
        }


        virtual bool selectParentRevision() override {
            // TODO: Implement this for the current revision if the CAS ancestor is available
            return false;
        }


        virtual bool selectNextRevision() override {
            return selectNextLeafRevision(false, false);
        }


        virtual bool selectNextLeafRevision(bool includeDeleted, bool withBody) override {
            loadRevisions();
            if (_revisions.size() == 0)
                return false;
            auto next = _revisions.begin();
            if (_selected && _selected != _current) {
                next = find(next, _revisions.end(), _selected);
                CBFAssert(next != _revisions.end());
                ++next;
            }
            if (next == _revisions.end())
                return false;
            selectRevision(*next);
            return true;
        }


        virtual bool hasRevisionBody() override {
            return _selected != nullptr;
        }


        virtual bool loadSelectedRevBodyIfAvailable() override {
            if (!_selected)
                return false;
            _store.readBody(*_selected);
            selectedRev.body = _selected->body();
            return true;
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
            return 1;
        }


        virtual bool putNewRevision(const C4DocPutRequest &rq) override {
            Revision::BodyParams bodyParams {rq.body, rq.docType, rq.deletion, rq.hasAttachments};
            shared_ptr<Revision> newRev = _store.create(rq.docID, _selected->version(), bodyParams,
                                                        _db->transaction());
            if (!newRev)
                return false;
            selectNewRev(newRev);
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


    C4DocumentV2::~C4DocumentV2() {
    }


    CASRevisionStore& c4DatabaseV2::revisionStore() {
        if (!_revisionStore)
            _revisionStore.reset(new CASRevisionStore(db()));
        return *_revisionStore;
    }


    C4DocumentInternal* c4DatabaseV2::newDocumentInstance(C4Slice docID) {
        return new C4DocumentV2(this, docID);
    }

    C4DocumentInternal* c4DatabaseV2::newDocumentInstance(const Document &doc) {
        return new C4DocumentV2(this, doc);
    }


    bool c4DatabaseV2::readDocMeta(const Document &doc,
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
