//
// BothKeyStore.hh
//
// Copyright © 2019 Couchbase. All rights reserved.
//

#pragma once
#include "KeyStore.hh"
#include "Query.hh"
#include "Error.hh"

namespace litecore {

    /** A fake KeyStore that combines a real KeyStore for live documents and another for tombstones,
        and makes them appear to be a single store.
        All live documents are in the live store; all deleted documents are in the dead store.
        Sequence numbers are shared across both stores. */
    class BothKeyStore : public KeyStore {
    public:
        BothKeyStore(KeyStore *liveStore NONNULL, KeyStore *deadStore NONNULL);

        KeyStore* liveStore() const                         {return _liveStore.get();}
        KeyStore* deadStore() const                         {return _deadStore.get();}

        void shareSequencesWith(KeyStore&) override         {Assert(false);}

        virtual uint64_t recordCount(bool includeDeleted =false) const override;
        
        virtual sequence_t lastSequence() const override    {return _liveStore->lastSequence();}
        virtual uint64_t purgeCount() const override        {return _liveStore->purgeCount();}


        //// CRUD:

        virtual bool read(Record &rec, ReadBy readBy, ContentOption content) const override {
            return _liveStore->read(rec, readBy, content) || _deadStore->read(rec, readBy, content);
        }


        virtual sequence_t set(const RecordUpdate &rec,
                               bool updateSequence,
                               ExclusiveTransaction &transaction) override;


        virtual void setKV(slice key,
                           slice version,
                           slice value,
                           ExclusiveTransaction &transaction) override
        {
            _liveStore->setKV(key, version, value, transaction);
        }

        virtual bool del(slice key, ExclusiveTransaction &t, sequence_t replacingSequence,
                         std::optional<uint64_t> replacingSubsequence =std::nullopt) override {
            // Always delete from both stores, for safety's sake.
            bool a = _liveStore->del(key, t, replacingSequence, replacingSubsequence);
            bool b = _deadStore->del(key, t, replacingSequence, replacingSubsequence);
            return a || b;
        }


        virtual bool setDocumentFlag(slice key, sequence_t seq, DocumentFlags flags,
                                     ExclusiveTransaction &t) override
        {
            return _liveStore->setDocumentFlag(key, seq, flags, t)
                || _deadStore->setDocumentFlag(key, seq, flags, t);
        }


        virtual void moveTo(slice key, KeyStore &dst, ExclusiveTransaction &t,
                            slice newKey =nullslice) override
        {
            _liveStore->moveTo(key, dst, t, newKey);
        }


        virtual void transactionWillEnd(bool commit) override {
            _liveStore->transactionWillEnd(commit);
            _deadStore->transactionWillEnd(commit);
        }


        //// EXPIRATION:

        virtual bool mayHaveExpiration() override {
            return _liveStore->mayHaveExpiration() || _deadStore->mayHaveExpiration();
        }


        virtual void addExpiration() override {
            _liveStore->addExpiration();
            _deadStore->addExpiration();
        }

        
        virtual bool setExpiration(slice key, expiration_t exp) override {
            return _liveStore->setExpiration(key, exp) || _deadStore->setExpiration(key, exp);
        }


        virtual expiration_t getExpiration(slice key) override {
            return std::max(_liveStore->getExpiration(key), _deadStore->getExpiration(key));
        }

        virtual expiration_t nextExpiration() override;


        virtual unsigned expireRecords(std::optional<ExpirationCallback> callback) override {
            return _liveStore->expireRecords(callback) + _deadStore->expireRecords(callback);
        }


        //// QUERIES & INDEXES:

        virtual std::vector<alloc_slice> withDocBodies(const std::vector<slice> &docIDs,
                                                       WithDocBodyCallback callback) override;

        virtual bool supportsIndexes(IndexSpec::Type type) const override {
            return _liveStore->supportsIndexes(type);
        }

        virtual bool createIndex(const IndexSpec &spec) override {
            return _liveStore->createIndex(spec);
        }


        virtual void deleteIndex(slice name) override {
            _liveStore->deleteIndex(name);
        }


        virtual std::vector<IndexSpec> getIndexes() const override {
            return _liveStore->getIndexes();
        }


    protected:
        virtual void reopen() override              {_liveStore->reopen(); _deadStore->reopen();}
        virtual void close() override               {_liveStore->close(); _deadStore->close();}
        virtual void deleteKeyStore() override      {_liveStore->deleteKeyStore(); _deadStore->deleteKeyStore();}
        virtual RecordEnumerator::Impl* newEnumeratorImpl(bool bySequence,
                                                          sequence_t since,
                                                          RecordEnumerator::Options) override;
    private:
        std::unique_ptr<KeyStore> _liveStore;
        std::unique_ptr<KeyStore> _deadStore;
    };


}
