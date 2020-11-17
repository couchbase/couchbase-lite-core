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

namespace c4Internal {


    class NuDocumentAdapter : public Document {
    public:
        NuDocumentAdapter(Database* database, C4Slice docID)
        :Document(database, docID)
        ,_versionedDoc(database->defaultKeyStore(), docID)
        {
        }

        NuDocumentAdapter(Database *database, const Record &doc)
        :Document(database, doc.key())
        ,_versionedDoc(database->defaultKeyStore(), doc)
        { }

        virtual bool exists() override {
            return _versionedDoc.exists();
        }

        bool _selectRemote(RemoteID remote) {
            auto rev = _versionedDoc.remoteRevision(remote);
            if (!rev)
                return false;
            _versionBuffer = rev->revID.expanded();
            selectedRev.revID = _versionBuffer;
            selectedRev.flags = currentRevFlagsFromDocFlags(C4DocumentFlags(rev->flags));
            selectedRev.body = nullptr;
            _remote = remote;
        }

        virtual bool selectRevision(C4Slice revID, bool withBody) override {

        }

        virtual bool selectParentRevision() noexcept override {

        }
        virtual bool selectNextRevision() override {

        }
        virtual bool selectNextLeafRevision(bool includeDeleted) override {

        }

        virtual alloc_slice remoteAncestorRevID(C4RemoteID) override {

        }

        virtual void setRemoteAncestorRevID(C4RemoteID) override {

        }

        virtual bool hasRevisionBody() noexcept override {
            return true;
        }

        virtual bool loadSelectedRevBody() override {
            return true;
        }

        virtual Retained<fleece::impl::Doc> fleeceDoc() override {
            return _versionedDoc.fleeceDoc();
        }

        virtual int32_t putExistingRevision(const C4DocPutRequest&, C4Error*) override {

        }

        virtual bool putNewRevision(const C4DocPutRequest&) override {

        }

        virtual bool save(unsigned maxRevTreeDepth =0) override {

        }

    private:
        NuDocument _versionedDoc;
        RemoteID _remote = RemoteID::Local;
        alloc_slice _versionBuffer;
    };


#pragma mark - FACTORY:


    Retained<Document> NuDocumentFactory::newDocumentInstance(C4Slice docID) {
        return new NuDocumentAdapter(database, docID);
    }


    Retained<Document> NuDocumentFactory::newDocumentInstance(const Record &record) {
        return new NuDocumentAdapter(database, record);
    }



    alloc_slice NuDocumentFactory::revIDFromVersion(slice version) {
        return revid(version).expanded();
    }



    vector<alloc_slice> NuDocumentFactory::findAncestors(const vector<slice> &docIDs,
                                              const vector<slice> &revIDs,
                                              unsigned maxAncestors,
                                              bool mustHaveBodies,
                                              C4RemoteID remoteDBID)
    {

    }


}
