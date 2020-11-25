//
// VersionedDocument.hh
//
// Copyright (c) 2014 Couchbase, Inc All rights reserved.
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

#pragma once
#include "RevTree.hh"
#include "Record.hh"
#include "Doc.hh"
#include "Logging.hh"
#include "StringUtil.hh"
#include <memory>
#include <vector>

namespace fleece { namespace impl {
    class Scope;
}}

namespace litecore {
    class KeyStore;
    class Transaction;

    /** Manages storage of a serialized RevTree in a Record. */
    class VersionedDocument : public RevTree {
    public:

        VersionedDocument(KeyStore&, slice docID);
        VersionedDocument(KeyStore&, const Record&);

        VersionedDocument(const VersionedDocument&);
        ~VersionedDocument();

        /** Reads and parses the body of the record. Useful if doc was read as meta-only. */
        void read();

        /** Returns false if the record was loaded metadata-only. Revision accessors will fail. */
        bool revsAvailable() const {return !_unknown;}

        const alloc_slice& docID() const {return _rec.key();}
        revid revID() const         {return revid(_rec.version());}
        DocumentFlags flags() const {return _rec.flags();}
        bool isDeleted() const      {return (flags() & DocumentFlags::kDeleted) != 0;}
        bool isConflicted() const   {return (flags() & DocumentFlags::kConflicted) != 0;}
        bool hasAttachments() const {return (flags() & DocumentFlags::kHasAttachments) != 0;}

        bool exists() const         {return _rec.exists();}
        sequence_t sequence() const {return _rec.sequence();}

        const Record& record() const    {return _rec;}

        bool changed() const        {return _changed;}

        enum SaveResult {kConflict, kNoNewSequence, kNewSequence};
        SaveResult save(Transaction& transaction);

        bool updateMeta();

        fleece::Retained<fleece::impl::Doc> fleeceDocFor(slice) const;

        /** Given a Fleece Value, finds the VersionedDocument it belongs to. */
        static VersionedDocument* containing(const fleece::impl::Value*);

        /** A pointer for clients to use */
        void* owner {nullptr};

#if DEBUG
        void dump()          {RevTree::dump();}
#endif
    protected:
        virtual alloc_slice copyBody(slice body) override;
        virtual alloc_slice copyBody(const alloc_slice &body) override;
#if DEBUG
        virtual void dump(std::ostream&) override;
#endif

    private:
        class VersFleeceDoc : public fleece::impl::Doc {
        public:
            VersFleeceDoc(const alloc_slice &fleeceData, fleece::impl::SharedKeys* sk,
                         VersionedDocument *document_)
            :fleece::impl::Doc(fleeceData, Doc::kDontParse, sk)
            ,document(document_)
            { }

            VersionedDocument* const document;
        };

        void decode();
        void updateScope();
        alloc_slice addScope(const alloc_slice &body);

        KeyStore&       _store;
        Record          _rec;
        std::vector<Retained<VersFleeceDoc>> _fleeceScopes;
    };
}
