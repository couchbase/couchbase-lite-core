//
// BothKeyStore.hh
//
// Copyright © 2019 Couchbase. All rights reserved.
//

#pragma once
#include "KeyStore.hh"
#include "Error.hh"

namespace litecore {

    /** A fake KeyStore that combines a real KeyStore for live documents and another for tombstones,
        and makes them appear to be a single store.
        All live documents are in the live store; all deleted documents are in the dead store.
        Sequence numbers are shared across both stores. */
    class BothKeyStore : public KeyStore {
      public:
        BothKeyStore(KeyStore* liveStore NONNULL, KeyStore* deadStore NONNULL);

        [[nodiscard]] KeyStore* liveStore() const { return _liveStore.get(); }

        [[nodiscard]] KeyStore* deadStore() const { return _deadStore.get(); }

        void shareSequencesWith(KeyStore&) override { Assert(false); }

        [[nodiscard]] uint64_t recordCount(bool includeDeleted = false) const override;

        [[nodiscard]] sequence_t lastSequence() const override { return _liveStore->lastSequence(); }

        [[nodiscard]] uint64_t purgeCount() const override { return _liveStore->purgeCount(); }

        //// CRUD:

        bool read(Record& rec, ReadBy readBy, ContentOption content) const override {
            return _liveStore->read(rec, readBy, content) || _deadStore->read(rec, readBy, content);
        }

        sequence_t set(const RecordUpdate& rec, SetOptions flags, ExclusiveTransaction& transaction) override;

        void setKV(slice key, slice version, slice value, ExclusiveTransaction& transaction) override {
            _liveStore->setKV(key, version, value, transaction);
        }

        bool del(slice key, ExclusiveTransaction& t, sequence_t replacingSequence,
                 std::optional<uint64_t> replacingSubsequence = std::nullopt) override {
            // Always delete from both stores, for safety's sake.
            bool a = _liveStore->del(key, t, replacingSequence, replacingSubsequence);
            bool b = _deadStore->del(key, t, replacingSequence, replacingSubsequence);
            return a || b;
        }

        bool setDocumentFlag(slice key, sequence_t seq, DocumentFlags flags, ExclusiveTransaction& t) override {
            return _liveStore->setDocumentFlag(key, seq, flags, t) || _deadStore->setDocumentFlag(key, seq, flags, t);
        }

        void moveTo(slice key, KeyStore& dst, ExclusiveTransaction& t, slice newKey = nullslice) override {
            _liveStore->moveTo(key, dst, t, newKey);
        }

        void transactionWillEnd(bool commit) override {
            _liveStore->transactionWillEnd(commit);
            _deadStore->transactionWillEnd(commit);
        }

        //// EXPIRATION:

        bool mayHaveExpiration() override { return _liveStore->mayHaveExpiration() || _deadStore->mayHaveExpiration(); }

        void addExpiration() override {
            _liveStore->addExpiration();
            _deadStore->addExpiration();
        }

        bool setExpiration(slice key, expiration_t exp) override {
            return _liveStore->setExpiration(key, exp) || _deadStore->setExpiration(key, exp);
        }

        expiration_t getExpiration(slice key) override {
            return std::max(_liveStore->getExpiration(key), _deadStore->getExpiration(key));
        }

        expiration_t nextExpiration() override;

        unsigned expireRecords(std::optional<ExpirationCallback> callback) override {
            return _liveStore->expireRecords(callback) + _deadStore->expireRecords(callback);
        }

        //// QUERIES & INDEXES:

        std::vector<alloc_slice> withDocBodies(const std::vector<slice>& docIDs, WithDocBodyCallback callback) override;

        [[nodiscard]] bool supportsIndexes(IndexSpec::Type type) const override {
            return _liveStore->supportsIndexes(type);
        }

        bool createIndex(const IndexSpec& spec) override { return _liveStore->createIndex(spec); }

        void deleteIndex(slice name) override { _liveStore->deleteIndex(name); }

        [[nodiscard]] std::vector<IndexSpec> getIndexes() const override { return _liveStore->getIndexes(); }


      protected:
        void reopen() override {
            _liveStore->reopen();
            _deadStore->reopen();
        }

        void close() override {
            _liveStore->close();
            _deadStore->close();
        }

        RecordEnumerator::Impl* newEnumeratorImpl(bool bySequence, sequence_t since,
                                                  RecordEnumerator::Options) override;

      private:
        std::unique_ptr<KeyStore> _liveStore;
        std::unique_ptr<KeyStore> _deadStore;
    };


}  // namespace litecore
