//
// RevTreeRecord.hh
//
// Copyright 2014-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "RevTree.hh"
#include "Record.hh"
#include "Doc.hh"
#include <memory>
#include <vector>

namespace fleece::impl {
    class Scope;
}  // namespace fleece::impl

namespace litecore {
    class KeyStore;
    class ExclusiveTransaction;

    /** Manages storage of a serialized RevTree in a Record. */
    class RevTreeRecord final : public RevTree {
      public:
        RevTreeRecord(KeyStore&, slice docID, ContentOption = kEntireBody);
        RevTreeRecord(KeyStore&, Record);

        RevTreeRecord(const RevTreeRecord&);
        ~RevTreeRecord() override;

        /** Reads and parses the body of the record. Useful if doc was read as meta-only.
            Returns false if the record has been updated on disk. */
        bool read(ContentOption) MUST_USE_RESULT;

        /** Returns false if the record was loaded metadata-only. Revision accessors will fail. */
        [[nodiscard]] bool revsAvailable() const { return _contentLoaded == kEntireBody; }

        [[nodiscard]] bool currentRevAvailable() const { return _contentLoaded >= kCurrentRevOnly; }

        [[nodiscard]] slice currentRevBody() const;

        [[nodiscard]] const alloc_slice& docID() const FLPURE { return _rec.key(); }

        [[nodiscard]] revid revID() const FLPURE { return revid(_rec.version()); }

        [[nodiscard]] DocumentFlags flags() const FLPURE { return _rec.flags(); }

        [[nodiscard]] bool isDeleted() const FLPURE { return (flags() & DocumentFlags::kDeleted) != 0; }

        [[nodiscard]] bool isConflicted() const FLPURE { return (flags() & DocumentFlags::kConflicted) != 0; }

        [[nodiscard]] bool hasAttachments() const FLPURE { return (flags() & DocumentFlags::kHasAttachments) != 0; }

        [[nodiscard]] bool exists() const FLPURE { return _rec.exists(); }

        [[nodiscard]] sequence_t sequence() const FLPURE { return _rec.sequence(); }

        [[nodiscard]] const Record& record() const FLPURE { return _rec; }

        [[nodiscard]] bool changed() const FLPURE { return _changed; }

        enum SaveResult { kConflict, kNoNewSequence, kNewSequence };

        SaveResult save(ExclusiveTransaction& transaction);

        bool updateMeta();

        [[nodiscard]] fleece::Retained<fleece::impl::Doc> fleeceDocFor(slice) const;

        /** Given a Fleece Value, finds the RevTreeRecord it belongs to. */
        static RevTreeRecord* containing(const fleece::impl::Value*);

        /** A pointer for clients to use */
        void* owner{nullptr};

      protected:
        alloc_slice copyBody(slice body) override;
        alloc_slice copyBody(const alloc_slice& body) override;
#if DEBUG
        void dump(std::ostream&) override;
#endif

      private:
        class VersFleeceDoc : public fleece::impl::Doc {
          public:
            VersFleeceDoc(const alloc_slice& fleeceData, fleece::impl::SharedKeys* sk, RevTreeRecord* document_)
                : fleece::impl::Doc(fleeceData, Doc::kDontParse, sk), document(document_) {}

            RevTreeRecord* const document;
        };

        void        decode();
        void        updateScope();
        alloc_slice addScope(const alloc_slice& body);

        KeyStore&                            _store;
        Record                               _rec;
        std::vector<Retained<VersFleeceDoc>> _fleeceScopes;
        ContentOption                        _contentLoaded{};
    };
}  // namespace litecore
