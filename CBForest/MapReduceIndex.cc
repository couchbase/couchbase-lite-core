//
//  MapReduceIndex.cc
//  CBForest
//
//  Created by Jens Alfke on 5/15/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
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


namespace forestdb {

    class emitter : public EmitFn {
    public:
        virtual void operator() (Collatable key, Collatable value);
        std::vector<Collatable> keys;
        std::vector<Collatable> values;
    };

    void emitter::operator() (Collatable key, Collatable value) {
        keys.push_back(key);
        values.push_back(value);
    }


    MapReduceIndex::MapReduceIndex(std::string path,
                                   forestdb::Database::openFlags flags,
                                   const forestdb::Database::config& config,
                                   forestdb::Database* sourceDatabase)
    :Index(path, flags, config),
     _sourceDatabase(sourceDatabase), _map(NULL), _indexType(0),
     _lastSequenceIndexed(0), _lastSequenceChangedAt(0), _stateReadAt(0), _rowCount(0)
    {
        readState();
    }

    void MapReduceIndex::readState() {
        sequence curIndexSeq = getInfo().last_seqnum;
        if (_stateReadAt != curIndexSeq) {
            Collatable stateKey;
            stateKey.addNull();

            Document state = get(stateKey);
            CollatableReader reader(state.body());
            if (reader.peekTag() == CollatableReader::kArray) {
                reader.beginArray();
                _lastSequenceIndexed = reader.readInt();
                _lastSequenceChangedAt = reader.readInt();
                _lastMapVersion = std::string(reader.readString());
                _indexType = (int)reader.readInt();
                _rowCount = (uint64_t)reader.readInt();
            }
            _stateReadAt = curIndexSeq;
        }
    }

    void MapReduceIndex::saveState(IndexTransaction& t) {
        _lastMapVersion = _mapVersion;

        Collatable stateKey;
        stateKey.addNull();

        Collatable state;
        state << _lastSequenceIndexed << _lastSequenceChangedAt << _lastMapVersion << _indexType
              << _rowCount;

        _stateReadAt = t.set(stateKey, state);
    }

    void MapReduceIndex::deleted() {
        _lastSequenceIndexed = 0;
        _lastSequenceChangedAt = 0;
        _lastMapVersion = "";
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


    void MapReduceIndex::setup(int indexType, MapFn *map, std::string mapVersion) {
        readState();
        if (indexType != _indexType || mapVersion != _lastMapVersion) {
            Transaction t(this);
            _map = map;
            _indexType = indexType;
            _mapVersion = mapVersion;
            t.erase();
        }
    }

    bool MapReduceIndex::updateDocInIndex(IndexTransaction& trans, const Mappable& mappable) {
        const Document& doc = mappable.document();
        if (doc.sequence() <= _lastSequenceIndexed)
            return false;
        emitter emit;
        if (!doc.deleted())
            (*_map)(mappable, emit); // Call map function!
        _lastSequenceIndexed = doc.sequence();
        if (trans.update(doc.key(), doc.sequence(), emit.keys, emit.values, _rowCount)) {
            _lastSequenceChangedAt = _lastSequenceIndexed;
            return true;
        }
        return false;
    }

    
#pragma mark - MAP-REDUCE INDEXER


    MapReduceIndexer::MapReduceIndexer(std::vector<MapReduceIndex*> indexes)
    :_indexes(indexes),
     _triggerIndex(NULL),
     _finished(false)
    { }

    bool MapReduceIndexer::run() {
        Database* sourceDatabase = _indexes[0]->sourceDatabase();
        sequence latestDbSequence = sourceDatabase->getInfo().last_seqnum;

        // First find the minimum sequence that not all indexes have indexed yet.
        // Also start a transaction for each index:
        sequence startSequence = latestDbSequence+1;
        for (auto idx = _indexes.begin(); idx != _indexes.end(); ++idx) {
            sequence lastSequence = (*idx)->lastSequenceIndexed();
            IndexTransaction* t = NULL;
            if (lastSequence < latestDbSequence) {
                startSequence = std::min(startSequence, lastSequence+1);
                t = new IndexTransaction(*idx);
            } else if (*idx == _triggerIndex) {
                return false; // The trigger index doesn't need to be updated, so abort
            }
            _transactions.push_back(t);
        }

        if (startSequence > latestDbSequence)
            return false; // no updating needed

        // Enumerate all the documents:
        DocEnumerator::Options options = DocEnumerator::Options::kDefault;
        options.includeDeleted = true;
        for (DocEnumerator e(sourceDatabase, startSequence, UINT64_MAX, options); e; ++e) {
            addDocument(*e);
        }
        _finished = true;
        return true;
    }

    MapReduceIndexer::~MapReduceIndexer() {
        // Save each index's state, and delete the transactions:
        const size_t n = _transactions.size();
        for (size_t i = 0; i < n; ++i) {
            if (_transactions[i]) {
                if (_finished)
                    _indexes[i]->saveState(*_transactions[i]);
                delete _transactions[i];
            }
        }
    }

    void MapReduceIndexer::addDocument(const Document& doc) {
        Mappable mappable(doc);
        addMappable(mappable);
    }

    void MapReduceIndexer::addMappable(const Mappable& mappable) {
        const size_t n = indexCount();
        for (size_t i = 0; i < n; ++i)
            updateDocInIndex(i, mappable);
    }

}
