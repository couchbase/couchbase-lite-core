//
//  MapReduceIndex.cc
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 5/15/14.
//  Copyright (c) 2014-2016 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#include "MapReduceIndex.hh"
#include "Collatable.hh"
#include "Error.hh"
#include "Fleece.hh"
#include "Logging.hh"
#include <algorithm>

namespace litecore {
    using namespace fleece;

    extern LogDomain IndexLog;

    static int64_t kMinFormatVersion = 6;
    static int64_t kCurFormatVersion = 6;

    MapReduceIndex::MapReduceIndex(KeyStore &store, DataFile &sourceDataFile)
    :Index(store),
     _sourceDataFile(sourceDataFile)
    {
        readState();
    }

    void MapReduceIndex::readState() {
        CollatableBuilder stateKey;
        stateKey.addNull();
        Record state = _store.get(stateKey);
        CollatableReader reader(state.body());
        if (reader.peekTag() == CollatableReader::kArray) {
            reader.beginArray();
            _lastSequenceIndexed = reader.readInt();
            _lastSequenceChangedAt = reader.readInt();
            _lastMapVersion = std::string(reader.readString());
            _indexType = (int)reader.readInt();
            _rowCount = (uint64_t)reader.readInt();

            if (reader.peekTag() == CollatableTypes::kEndSequence
                    || reader.readInt() < kMinFormatVersion) {
                // Obsolete index version
                deleted();
                _indexType = 0;
            }
            if (reader.peekTag() != CollatableTypes::kEndSequence)
                _lastPurgeCount = (uint64_t)reader.readInt();
        }
        LogToAt(IndexLog, Debug, "MapReduceIndex<%p>: Read state (lastSeq=%llu, lastChanged=%llu, lastMapVersion='%s', indexType=%d, rowCount=%llu, lastPurgeCount=%llu)",
              this, (unsigned long long)_lastSequenceIndexed, (unsigned long long)_lastSequenceChangedAt, _lastMapVersion.c_str(), _indexType, (unsigned long long)_rowCount, (unsigned long long)_lastPurgeCount);
    }

    void MapReduceIndex::saveState(Transaction& t) {
        Assert(&_store.dataFile() == &t.dataFile());
        _lastMapVersion = _mapVersion;

        CollatableBuilder stateKey;
        stateKey.addNull();

        CollatableBuilder state;
        state.beginArray();
        state << _lastSequenceIndexed << _lastSequenceChangedAt << _lastMapVersion << _indexType
              << _rowCount << kCurFormatVersion << _lastPurgeCount;
        state.endArray();

        _stateReadAt = _store.set(stateKey, state, t).seq;
        LogToAt(IndexLog, Debug, "MapReduceIndex<%p>: Saved state (lastSeq=%llu, lastChanged=%llu, lastMapVersion='%s', indexType=%d, rowCount=%llu, lastPurgeCount=%llu)",
              this, (unsigned long long)_lastSequenceIndexed, (unsigned long long)_lastSequenceChangedAt, _lastMapVersion.c_str(), _indexType, (unsigned long long)_rowCount, (unsigned long long)_lastPurgeCount);
    }

    void MapReduceIndex::deleted() {
        _lastSequenceIndexed = 0;
        _lastSequenceChangedAt = 0;
        _lastMapVersion = "";
        _lastPurgeCount = 0;
        _stateReadAt = 0;
        _rowCount = 0;
    }

    sequence MapReduceIndex::lastSequenceIndexed() const {
        const_cast<MapReduceIndex*>(this)->readState();
        return _lastSequenceIndexed;
    }

    sequence MapReduceIndex::lastSequenceChangedAt() const {
        const_cast<MapReduceIndex*>(this)->readState();
        return _lastSequenceChangedAt;
    }

    uint64_t MapReduceIndex::rowCount() const {
        const_cast<MapReduceIndex*>(this)->readState();
        return _rowCount;
    }


    // Checks the index's saved purgeCount against the db's current purgeCount;
    // if they don't match, the index is invalidated (erased).
    bool MapReduceIndex::checkForPurge() {
        readState();
        auto dbPurgeCount = _sourceDataFile.purgeCount();
        if (dbPurgeCount == _lastPurgeCount)
            return false;
        invalidate();
        _lastPurgeCount = dbPurgeCount;
        return true;
    }


    void MapReduceIndex::setup(int indexType, std::string mapVersion) {
        LogToAt(IndexLog, Debug, "MapReduceIndex<%p>: Setup (indexType=%d, mapVersion='%s')",
              this, indexType, mapVersion.c_str());
        readState();
        _mapVersion = mapVersion;
        if (indexType != _indexType || mapVersion != _lastMapVersion) {
            _indexType = indexType;
            invalidate();
        }
    }

    void MapReduceIndex::invalidate() {
        if (_lastSequenceIndexed > 0) {
            LogToAt(IndexLog, Debug, "MapReduceIndex: Erasing invalidated index");
            _store.erase();
        }
        _lastSequenceIndexed = _lastSequenceChangedAt = _lastPurgeCount = 0;
        _rowCount = 0;
        _stateReadAt = 0;
    }

    void MapReduceIndex::erase() {
        LogToAt(IndexLog, Debug, "MapReduceIndex: Erasing");
        _store.erase();
        _lastSequenceIndexed = _lastSequenceChangedAt = _lastPurgeCount = 0;
        _rowCount = 0;
        _stateReadAt = 0;
    }

    alloc_slice MapReduceIndex::getSpecialEntry(slice recordID, sequence seq, unsigned entryID) const
    {
        // This data was written by emitSpecial
        CollatableBuilder key;
        key.addNull();
        return getEntry(recordID, seq, key, entryID);
    }


#pragma mark - EMITTER:


    // Collects key/value pairs being emitted
    class Emitter {
    public:

        std::vector<Collatable> keys;
        std::vector<alloc_slice> values;

        void emit(Collatable key, alloc_slice value) {
            keys.push_back(key);
            values.push_back(value);
        }

        void reset() {
            keys.clear();
            values.clear();
        }

    };


#pragma mark - INDEX WRITER:


    // In charge of updating one view's index. Owned by a MapReduceIndexer.
    class MapReduceIndexWriter: IndexWriter {
    public:
        MapReduceIndexWriter(MapReduceIndex &idx, Transaction *t)
        :IndexWriter(idx, *t, (idx.rowCount() == 0)),
         index(idx),
         _documentType(index.docType()),
         _transaction(t)
        { }

        MapReduceIndex & index;

        bool shouldIndexRecord(const Record& rec) const noexcept {
            return rec.sequence() > index._lastSequenceIndexed;
        }

        bool shouldIndexDocumentType(slice documentType) noexcept {
            return _documentType.buf == nullptr || _documentType == documentType;
        }

        // Writes the given rows to the index.
        bool indexRecord(slice recordID,
                           sequence recordSequence,
                           const std::vector<Collatable> &keys,
                           const std::vector<alloc_slice> &values)
        {
            if (recordSequence <= index._lastSequenceIndexed)
                return false;
            _emitter.reset();
            for (unsigned i = 0; i < keys.size(); ++i)
                _emitter.emit(keys[i], values[i]);

            index._lastSequenceIndexed = recordSequence;
            if (update(recordID, recordSequence, _emitter.keys, _emitter.values, index._rowCount)) {
                index._lastSequenceChangedAt = index._lastSequenceIndexed;
                return true;
            }
            return false;
        }

        void finish(sequence finalSequence) {
            if (finalSequence > 0) {
                index._lastSequenceIndexed = std::max(index._lastSequenceIndexed,
                                                      finalSequence);
                index.saveState(*_transaction);
                _transaction->commit();
            } else {
                _transaction->abort();
            }
        }

    private:
        alloc_slice const _documentType;
        Emitter _emitter;
        std::unique_ptr<Transaction> _transaction;
    };

    
#pragma mark - MAP-REDUCE INDEXER

    
    MapReduceIndexer::MapReduceIndexer() { }

    MapReduceIndexer::~MapReduceIndexer() { }

    void MapReduceIndexer::addIndex(MapReduceIndex &index) {
        index.checkForPurge(); // has to be called before creating the transaction
        auto writer = new MapReduceIndexWriter(index, new Transaction(index.dataFile()));
        _writers.emplace_back(writer);
        if (index.docType().buf)
            _docTypes.insert(index.docType());
        else
            _allDocTypes = true;
    }


    sequence MapReduceIndexer::startingSequence() {
        _latestDbSequence = _writers[0]->index.sourceStore().lastSequence();

        // First find the minimum sequence that not all indexes have indexed yet.
        sequence startSequence = _latestDbSequence+1;
        for (auto &writer : _writers) {
            sequence lastSequence = writer->index.lastSequenceIndexed();
            if (lastSequence < _latestDbSequence) {
                startSequence = std::min(startSequence, lastSequence+1);
            } else if (&writer->index == _triggerIndex) {
                return UINT64_MAX; // The trigger index doesn't need to be updated, so abort
            }
        }
        if (startSequence > _latestDbSequence)
            startSequence = UINT64_MAX; // no updating needed
        return startSequence;
    }

    std::set<slice>* MapReduceIndexer::documentTypes() {
        return _allDocTypes ? nullptr : &_docTypes;
    }


    void MapReduceIndexer::finished(sequence seq) {
        for (auto writer = _writers.begin(); writer != _writers.end(); ++writer) {
            (*writer)->finish(seq);
        }
    }

    bool MapReduceIndexer::shouldMapDocIntoView(const Record &rec, unsigned viewNumber) noexcept {
        return _writers[viewNumber]->shouldIndexRecord(rec);
    }

    bool MapReduceIndexer::shouldMapDocTypeIntoView(slice docType, unsigned viewNumber) noexcept {
        return _writers[viewNumber]->shouldIndexDocumentType(docType);
    }

    void MapReduceIndexer::emitDocIntoView(slice recordID,
                                           sequence recordSequence,
                                           unsigned viewNumber,
                                           const std::vector<Collatable> &keys,
                                           const std::vector<alloc_slice> &values)
    {
        _writers[viewNumber]->indexRecord(recordID, recordSequence, keys, values);
    }

    void MapReduceIndexer::skipDoc(slice recordID, sequence recordSequence) {
        for (auto &writer : _writers)
            writer->indexRecord(recordID, recordSequence, _noKeys, _noValues);
    }

    void MapReduceIndexer::skipDocInView(slice recordID, sequence recordSequence, unsigned viewNumber) {
        _writers[viewNumber]->indexRecord(recordID, recordSequence, _noKeys, _noValues);
    }

}
