//
// BothKeyStore.cc
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

#include "BothKeyStore.hh"
#include "DataFile.hh"
#include "RecordEnumerator.hh"
#include <memory>

namespace litecore {
    using namespace std;

    BothKeyStore::BothKeyStore(KeyStore* liveStore, KeyStore* deadStore)
        : KeyStore(liveStore->dataFile(), liveStore->name(), liveStore->capabilities())
        , _liveStore(liveStore)
        , _deadStore(deadStore) {
        deadStore->shareSequencesWith(*liveStore);
    }

    uint64_t BothKeyStore::recordCount(bool includeDeleted) const {
        bool isDefaultStore = (name() == DataFile::kDefaultKeyStoreName);
        // For default keystore, _liveStore may contain deleted docs. We pass includeDeleted to _liveStore to
        // filter out the deleted ones. CBL-4377
        // For non-default stores, true is faster, and there are none anyway
        auto count = _liveStore->recordCount(includeDeleted || !isDefaultStore);
        if ( includeDeleted ) count += _deadStore->recordCount(true);
        return count;
    }

    sequence_t BothKeyStore::set(const RecordUpdate& rec, SetOptions flags, ExclusiveTransaction& t) {
        bool deleting = (rec.flags & DocumentFlags::kDeleted);
        auto target   = (deleting ? _deadStore : _liveStore).get();  // the store to update
        auto other    = (deleting ? _liveStore : _deadStore).get();

        auto inserting = (rec.sequence == 0_seq || flags & kInsert);

        // At this level, insertion of a new record must pick a new sequence.
        Assert(flags & kUpdateSequence || !inserting);

        if ( inserting ) {
            // Request should succeed only if doc _doesn't_ exist yet, so check other KeyStore:
            if ( other->get(rec.key, kMetaOnly).exists() ) return 0_seq;
        }

        // Forward the 'set' to the target store:
        auto seq = target->set(rec, flags, t);

        if ( seq == 0_seq && rec.sequence > 0_seq ) {
            // Conflict. Maybe record is currently in the other KeyStore; if so, delete it & retry
            expiration_t expiry = other->getExpiration(rec.key);
            if ( other->del(rec.key, t, rec.sequence, rec.subsequence) ) {
                // We move a record from one sub-store to the other one by deleting it from
                // one store and inserting it to the other one while keeping the seqquence.
                seq = target->set(rec, SetOptions(flags | kInsert), t);
                if ( seq != sequence_t::None && expiry != expiration_t::None ) {
                    target->setExpiration(rec.key, expiry);
                }
            }
        }
        return seq;
    }

    std::vector<alloc_slice> BothKeyStore::withDocBodies(const std::vector<slice>& docIDs,
                                                         WithDocBodyCallback       callback) {
        // First, delegate to the live store:
        size_t nDocs  = docIDs.size();
        auto   result = _liveStore->withDocBodies(docIDs, callback);

        // Collect the docIDs that weren't found in the live store:
        std::vector<slice>  recheckDocs;
        std::vector<size_t> recheckIndexes(nDocs);
        size_t              nRecheck = 0;
        for ( size_t i = 0; i < nDocs; ++i ) {
            if ( !result[i] ) {
                recheckDocs.push_back(docIDs[i]);
                recheckIndexes[nRecheck++] = i;
            }
        }

        // Retry those docIDs in the dead store and add any results:
        if ( nRecheck > 0 ) {
            auto dead = _deadStore->withDocBodies(recheckDocs, callback);
            for ( size_t i = 0; i < nRecheck; ++i ) {
                if ( dead[i] ) result[recheckIndexes[i]] = dead[i];
            }
        }

        return result;
    }

    expiration_t BothKeyStore::nextExpiration() {
        auto lx = _liveStore->nextExpiration();
        auto dx = _deadStore->nextExpiration();
        if ( lx > expiration_t::None && dx > expiration_t::None ) return std::min(lx, dx);  // choose the earliest time
        else
            return std::max(lx, dx);  // or choose the nonzero time
    }

#pragma mark - ENUMERATOR:

    template <typename T>
    static inline int compare(T a, T b) {
        return (a < b) ? -1 : ((a > b) ? 1 : 0);
    }

    // Enumerator implementation for BothKeyStore when `includeDeleted` option is set
    // and sorting is required.
    // It enumerates both KeyStores in parallel,
    // always returning the lowest-sorting record (basically a merge-sort.)
    class BothEnumeratorImpl final : public RecordEnumerator::Impl {
      public:
        BothEnumeratorImpl(RecordEnumerator::Options options, KeyStore* liveStore, KeyStore* deadStore)
            : _liveImpl(liveStore->newEnumeratorImpl(options))
            , _deadImpl(deadStore->newEnumeratorImpl(options))
            , _bySequence(options.minSequence > 0_seq)
            , _descending(options.sortOption == kDescending) {}

        bool next() override {
            // Advance the enumerator with the lowest key, or both if they're equal:
            if ( _cmp <= 0 ) {
                if ( !_liveImpl->next() ) _liveImpl.reset();
            }
            if ( _cmp >= 0 ) {
                if ( !_deadImpl->next() ) _deadImpl.reset();
            }

            // Compare the enumerators' keys or sequences:
            if ( _liveImpl && _deadImpl ) {
                if ( _bySequence ) _cmp = compare(_liveImpl->sequence(), _deadImpl->sequence());
                else
                    _cmp = _liveImpl->key().compare(_deadImpl->key());
                if ( _descending ) _cmp = -_cmp;
            } else if ( _liveImpl ) {
                _cmp = -1;
            } else if ( _deadImpl ) {
                _cmp = 1;
            } else {
                // finished
                _cmp     = 0;
                _current = nullptr;
                return false;
            }

            // Pick the enumerator with the lowest key/sequence to be used next.
            // In case of a tie, pick the live one since it has priority.
            _current = ((_cmp <= 0) ? _liveImpl : _deadImpl).get();
            return true;
        }

        bool read(Record& record) const override { return _current->read(record); }

        [[nodiscard]] slice key() const override { return _current->key(); }

        [[nodiscard]] sequence_t sequence() const override { return _current->sequence(); }

      private:
        unique_ptr<RecordEnumerator::Impl> _liveImpl, _deadImpl;      // Real enumerators
        RecordEnumerator::Impl*            _current{nullptr};         // Enumerator w/lowest key
        int                                _cmp{0};                   // Comparison of live to dead
        bool                               _bySequence, _descending;  // Sorting by sequence?
    };

    // Enumerator implementation for BothKeyStore when `includeDeleted` option is set
    // but no sorting is needed. It simply enumerates the live store first, then the deleted.
    // This avoids having to sort the underlying SQLite queries, which enables better use of
    // indexes in `onlyConflicts` mode.
    class BothUnorderedEnumeratorImpl final : public RecordEnumerator::Impl {
      public:
        BothUnorderedEnumeratorImpl(RecordEnumerator::Options const& options, KeyStore* liveStore, KeyStore* deadStore)
            : _impl(liveStore->newEnumeratorImpl(options)), _deadStore(deadStore), _options(options) {}

        bool next() override {
            bool ok = _impl->next();
            if ( !ok && _deadStore != nullptr ) {
                _impl      = unique_ptr<RecordEnumerator::Impl>(_deadStore->newEnumeratorImpl(_options));
                _deadStore = nullptr;
                ok         = _impl->next();
            }
            return ok;
        }

        bool read(Record& record) const override { return _impl->read(record); }

        [[nodiscard]] slice key() const override { return _impl->key(); }

        [[nodiscard]] sequence_t sequence() const override { return _impl->sequence(); }

      private:
        unique_ptr<RecordEnumerator::Impl> _impl;       // Current enumerator
        KeyStore*                          _deadStore;  // The deleted store, before I switch to it
        RecordEnumerator::Options          _options;    // Enumerator options
    };

    RecordEnumerator::Impl* BothKeyStore::newEnumeratorImpl(RecordEnumerator::Options const& options) {
        bool isDefaultStore = (name() == DataFile::kDefaultKeyStoreName);
        if ( options.includeDeleted ) {
            if ( options.sortOption == kUnsorted )
                return new BothUnorderedEnumeratorImpl(options, _liveStore.get(), _deadStore.get());
            else
                return new BothEnumeratorImpl(options, _liveStore.get(), _deadStore.get());
        } else {
            auto optionsCopy = options;
            if ( !isDefaultStore ) {
                // For non default store, liveStore contains only live records. By assigning
                // includeDeleted to true, we won't apply flag filter to filter out the deleted.
                // For default store, however, liveStore may have deleted records.
                optionsCopy.includeDeleted = true;  // no need for enum to filter out deleted docs
            }
            return _liveStore->newEnumeratorImpl(optionsCopy);
        }
    }


}  // namespace litecore
