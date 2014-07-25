//
//  MapReduceIndex.cc
//  CBForest
//
//  Created by Jens Alfke on 5/15/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

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
     _lastSequenceIndexed(0), _lastSequenceChangedAt(0), _stateReadAt(0)
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
            }
            _stateReadAt = curIndexSeq;
        }
    }

    void MapReduceIndex::saveState(IndexTransaction& t) {
        Collatable stateKey;
        stateKey.addNull();

        Collatable state;
        state << _lastSequenceIndexed << _lastSequenceChangedAt << _lastMapVersion << _indexType;

        _stateReadAt = t.set(stateKey, state);
    }

    void MapReduceIndex::invalidate() {
        _lastSequenceIndexed = 0;
        _lastSequenceChangedAt = 0;
        _lastMapVersion = "";
        _stateReadAt = 0;
    }

    sequence MapReduceIndex::lastSequenceIndexed() const {
        const_cast<MapReduceIndex*>(this)->readState();
        return _lastSequenceIndexed;
    }

    sequence MapReduceIndex::lastSequenceChangedAt() const {
        const_cast<MapReduceIndex*>(this)->readState();
        return _lastSequenceChangedAt;
    }


    void MapReduceIndex::setup(int indexType, MapFn *map, std::string mapVersion) {
        readState();
        if (indexType != _indexType || mapVersion != _lastMapVersion) {
            Transaction t(this);
            _map = map;
            _indexType = indexType;
            _mapVersion = mapVersion;
            t.erase();
            invalidate();
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
        if (trans.update(doc.key(), doc.sequence(), emit.keys, emit.values)) {
            _lastSequenceChangedAt = _lastSequenceIndexed;
            return true;
        }
        return false;
    }

    
#pragma mark - MAP-REDUCE INDEXER


    MapReduceIndexer::MapReduceIndexer(std::vector<MapReduceIndex*> indexes)
    :_indexes(indexes)
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
        return true;
    }

    MapReduceIndexer::~MapReduceIndexer() {
        // Save each index's state, and delete the transactions:
        const size_t n = _indexes.size();
        for (size_t i = 0; i < n; ++i) {
            if (_transactions[i]) {
                _indexes[i]->_lastMapVersion = _indexes[i]->_mapVersion;
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
        const size_t n = _indexes.size();
        for (size_t i = 0; i < n; ++i) {
            if (_transactions[i])
                _indexes[i]->updateDocInIndex(*_transactions[i], mappable);
        }
    }

}