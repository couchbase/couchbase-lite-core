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
        if (indexType != _indexType || mapVersion != _lastMapVersion) {
            Transaction t(this);
            _map = map;
            _indexType = indexType;
            _mapVersion = mapVersion;
            t.erase();
            invalidate();
        }
    }

    bool MapReduceIndex::updateDocInIndex(IndexTransaction& trans, const Document& doc) {
        emitter emit;
        if (!doc.deleted())
            (*_map)(doc, emit); // Call map function!
        _lastSequenceIndexed = doc.sequence();
        if (trans.update(doc.key(), doc.sequence(), emit.keys, emit.values)) {
            _lastSequenceChangedAt = _lastSequenceIndexed;
            return true;
        }
        return false;
    }

    
    void MapReduceIndex::updateIndex() {
        IndexTransaction trans(this);

        if (_lastMapVersion.size() && _lastMapVersion != _mapVersion) {
            trans.erase();
            invalidate();
        }

        sequence startSequence = _lastSequenceIndexed + 1;

        DocEnumerator::Options options = DocEnumerator::Options::kDefault;
        options.includeDeleted = true;
        for (DocEnumerator e(_sourceDatabase, startSequence, UINT64_MAX, options); e; ++e) {
            updateDocInIndex(trans, e.doc());
        }

        _lastMapVersion = _mapVersion;
        if (_lastSequenceIndexed >= startSequence)
            saveState(trans);
    }


    void MapReduceIndex::updateMultipleIndexes(std::vector<MapReduceIndex*> indexes) {
        const size_t n = indexes.size();
        std::vector<sequence> lastSequences;
        std::vector<IndexTransaction*> transactions;
        Database* sourceDatabase = indexes[0]->_sourceDatabase;
        sequence latestDbSequence = sourceDatabase->getInfo().last_seqnum;

        // First find the minimum sequence that not all indexes have indexed yet.
        // Also start a transaction for each index:
        sequence startSequence = latestDbSequence+1;
        for (auto idx = indexes.begin(); idx != indexes.end(); ++idx) {
            sequence lastSequence = (*idx)->lastSequenceIndexed();
            lastSequences.push_back(lastSequence);
            startSequence = std::min(startSequence, lastSequence+1);

            IndexTransaction* t = NULL;
            if (lastSequence < latestDbSequence)
                t = new IndexTransaction(*idx);
            transactions.push_back(t);
        }

        if (startSequence >= latestDbSequence)
            return; // no updating needed

        // Enumerate all the documents:
        DocEnumerator::Options options = DocEnumerator::Options::kDefault;
        options.includeDeleted = true;
        for (DocEnumerator e(sourceDatabase, startSequence, UINT64_MAX, options); e; ++e) {
            
            for (size_t i = 0; i < n; ++i) {
                if (e->sequence() > lastSequences[i])
                    indexes[i]->updateDocInIndex(*transactions[i], e.doc());
            }
        }

        // Save each index's state, and delete the transactions:
        for (size_t i = 0; i < n; ++i) {
            if (transactions[i]) {
                indexes[i]->_lastMapVersion = indexes[i]->_mapVersion;
                indexes[i]->saveState(*transactions[i]);
                delete transactions[i];
            }
        }
    }

}