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
     _lastSequenceIndexed(0), _lastSequenceChangedAt(0)
    {
        readState();
    }

    void MapReduceIndex::readState() {
        Collatable stateKey;
        stateKey.addNull();

        Document state = get(stateKey);
        CollatableReader reader(state.body());
        if (reader.nextTag() == CollatableReader::kArray) {
            reader.beginArray();
            _lastSequenceIndexed = reader.readInt();
            _lastSequenceChangedAt = reader.readInt();
            _lastMapVersion = std::string(reader.readString());
            _indexType = (int)reader.readInt();
        }
    }

    void MapReduceIndex::saveState(IndexTransaction& t) {
        Collatable stateKey;
        stateKey.addNull();

        Collatable state;
        state << _lastSequenceIndexed << _lastSequenceChangedAt << _lastMapVersion << _indexType;

        t.set(stateKey, state);
    }

    void MapReduceIndex::invalidate() {
        _lastSequenceIndexed = 0;
        _lastSequenceChangedAt = 0;
        _lastMapVersion = "";
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

    void MapReduceIndex::updateIndex() {
        IndexTransaction trans(this);

        if (_lastMapVersion.size() && _lastMapVersion != _mapVersion) {
            trans.erase();
            invalidate();
        }

        sequence startSequence = _lastSequenceIndexed + 1;
        bool indexChanged = false;

        DocEnumerator::Options options = {
            .includeDeleted = true
        };
        for (DocEnumerator e(_sourceDatabase, startSequence, UINT64_MAX, options); e; ++e) {
            emitter emit;
            if (!e.doc().deleted())
                (*_map)(e.doc(), emit); // Call map function!
            if (trans.update(e->key(), e->sequence(), emit.keys, emit.values))
                indexChanged = true;
            _lastSequenceIndexed = e->sequence();
        }

        _lastMapVersion = _mapVersion;
        if (_lastSequenceIndexed >= startSequence) {
            if (indexChanged)
                _lastSequenceChangedAt = _lastSequenceIndexed;
            saveState(trans);
        }
    }

}