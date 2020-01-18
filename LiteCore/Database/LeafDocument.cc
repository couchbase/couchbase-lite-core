//
// LeafDocument.cc
//
// Copyright © 2019 Couchbase. All rights reserved.
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
#include "Database.hh"
#include "VersionedDocument.hh"
#include "RevID.hh"
#include "Doc.hh"

using namespace fleece;
using namespace fleece::impl;

namespace c4Internal {

    // Fleece Doc that tracks an associated Document
    class LeafFleeceDoc : public fleece::impl::Doc {
    public:
        LeafFleeceDoc(const alloc_slice &fleeceData, Trust trust,
                      SharedKeys* sk, Document *doc) noexcept
        :Doc(fleeceData, trust, sk)
        ,c4doc(doc)
        { }

        Document* const c4doc;
    };


    // A Document implementation that only stores the body of the current revision.
    class LeafDocument : public Document {
    public:
        LeafDocument(Database *database, slice docID_, slice revID_, bool withBody)
        :Document(database, docID_)
        {
            ContentOption options = withBody ? kCurrentRevOnly : kMetaOnly;
            database->defaultKeyStore().get(docID_, options, [&](const Record &record) {
                if (record.exists()) {
                    _fleeceDoc = new LeafFleeceDoc(record.body(),
                                                   Doc::kTrusted,
                                                   database->documentKeys(),
                                                   this);
                    setRevID(revid(record.version()));
                    flags = C4DocumentFlags(record.flags()) | kDocExists;
                    sequence = record.sequence();
                } else {
                    flags = 0;
                    sequence = 0;
                }
            });
            if (revID_ && revID_ != slice(revID))
                failUnsupported();              //TODO: Implement loading non-current revisions
            selectCurrentRevision();
        }


        virtual Document* copy() override {
            return new LeafDocument(*this);
        }

        virtual bool exists() override  {
            return flags & kDocExists;
        }

        virtual bool revisionsLoaded() const noexcept override  {
            return false;
        }

        virtual bool selectCurrentRevision() noexcept override {
            Document::selectCurrentRevision();
            selectedRev.body = _fleeceDoc ? _fleeceDoc->allocedData() : nullslice;
            return exists();
        }

        virtual bool selectRevision(C4Slice selectRevID, bool withBody) override {
            if (slice(selectRevID) != slice(revID))
                failUnsupported();
            return true;
        }

        virtual bool hasRevisionBody() noexcept override {
            return _fleeceDoc != nullptr;
        }

        virtual bool loadSelectedRevBody() override {
            if (!_fleeceDoc)
                failUnsupported();
            return true;
        }

        virtual Retained<fleece::impl::Doc> fleeceDoc() override {
            return _fleeceDoc;
        }

        // These always fail because I don't have the whole rev tree:
        virtual void loadRevisions() override                               {failUnsupported();}
        virtual bool selectParentRevision() noexcept override               {return false;}
        virtual bool selectNextRevision() override                          {failUnsupported();}
        virtual bool selectNextLeafRevision(bool includeDeleted) override   {failUnsupported();}
        virtual alloc_slice remoteAncestorRevID(C4RemoteID) override        {failUnsupported();}
        virtual void setRemoteAncestorRevID(C4RemoteID) override            {failUnsupported();}
        virtual bool save(unsigned maxRevTreeDepth =0) override             {failUnsupported();}
        virtual bool putNewRevision(const C4DocPutRequest&) override        {failUnsupported();}
        virtual int32_t putExistingRevision(const C4DocPutRequest&, C4Error*) override{failUnsupported();}
    private:
        Retained<LeafFleeceDoc> _fleeceDoc;
    };


#pragma mark - DOCUMENT FACTORY METHODS:

    // Bodies of TreeDocumentFactory methods that create LeafDocuments.


    Retained<Document> TreeDocumentFactory::newLeafDocumentInstance(C4Slice docID, C4Slice revID,
                                                                    bool withBody)
    {
        if (revID.buf) {
            // TODO: Implement LeafDocument ability to load a specific revision
            auto doc = newDocumentInstance(docID);
            if (!doc->selectRevision(revID, withBody))
                doc = nullptr;
            return doc;
        } else {
            return new LeafDocument(database(), docID, revID, withBody);
        }
    }

    Document* TreeDocumentFactory::leafDocumentContaining(const Value *value) {
        const Doc *doc = fleece::impl::Doc::containing(value);
        if (!doc)
            return nullptr;
        auto leafDoc = dynamic_cast<const LeafFleeceDoc*>(doc);
        if (!leafDoc)
            return nullptr;
        return leafDoc->c4doc;
    }


}
