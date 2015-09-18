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
#include "GeoIndex.hh"
#include "Tokenizer.hh"
#include "LogInternal.hh"
#include <assert.h>
#include <algorithm>

namespace forestdb {

    static int64_t kMinFormatVersion = 3;
    static int64_t kCurFormatVersion = 3;

    MapReduceIndex::MapReduceIndex(Database* db, std::string name, KeyStore sourceStore)
    :Index(db, name),
     _sourceDatabase(sourceStore), _map(NULL), _indexType(0),
     _lastSequenceIndexed(0), _lastSequenceChangedAt(0), _stateReadAt(0), _rowCount(0)
    {
        readState();
    }

    void MapReduceIndex::readState() {
        sequence curIndexSeq = KeyStore::lastSequence();
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

                if (reader.peekTag() == CollatableTypes::kEndSequence
                        || reader.readInt() < kMinFormatVersion) {
                    // Obsolete index version
                    deleted();
                    _indexType = 0;
                }
            }
            _stateReadAt = curIndexSeq;
            Debug("MapReduceIndex<%p>: Read state (lastSeq=%lld, lastChanged=%lld, lastMapVersion='%s', indexType=%d, rowCount=%d)",
                  this, _lastSequenceIndexed, _lastSequenceChangedAt, _lastMapVersion.c_str(), _indexType, _rowCount);
        }
    }

    void MapReduceIndex::saveState(Transaction& t) {
        assert(t.database()->contains(*this));
        _lastMapVersion = _mapVersion;

        Collatable stateKey;
        stateKey.addNull();

        Collatable state;
        state.beginArray();
        state << _lastSequenceIndexed << _lastSequenceChangedAt << _lastMapVersion << _indexType
              << _rowCount << kCurFormatVersion;
        state.endArray();

        _stateReadAt = t(this).set(stateKey, state);
        Debug("MapReduceIndex<%p>: Saved state (lastSeq=%lld, lastChanged=%lld, lastMapVersion='%s', indexType=%d, rowCount=%d)",
              this, _lastSequenceIndexed, _lastSequenceChangedAt, _lastMapVersion.c_str(), _indexType, _rowCount);
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


    void MapReduceIndex::setup(Transaction &t, int indexType, MapFn *map, std::string mapVersion) {
        Debug("MapReduceIndex<%p>: Setup (indexType=%ld, mapFn=%p, mapVersion='%s')",
              this, indexType, map, mapVersion.c_str());
        assert(t.database()->contains(*this));
        assert(map != NULL);
        readState();
        _map = map;
        _mapVersion = mapVersion;
        if (indexType != _indexType || mapVersion != _lastMapVersion) {
            _indexType = indexType;
            if (_lastSequenceIndexed > 0) {
                Debug("MapReduceIndex: Version or indexType changed; erasing");
                KeyStore::erase(t);
            }
            _lastSequenceIndexed = _lastSequenceChangedAt = 0;
            _rowCount = 0;
            _stateReadAt = 0;
        }
    }

    void MapReduceIndex::erase(Transaction& t) {
        Debug("MapReduceIndex: Erasing");
        assert(t.database()->contains(*this));
        KeyStore::erase(t);
        _lastSequenceIndexed = _lastSequenceChangedAt = 0;
        _rowCount = 0;
        _stateReadAt = 0;
    }

    alloc_slice MapReduceIndex::getSpecialEntry(slice docID, sequence seq, unsigned entryID)
    {
        // This data was written by emitter::emitTextTokens, below
        Collatable key;
        key.addNull();
        return getEntry(docID, seq, key, entryID);
    }

    alloc_slice MapReduceIndex::readFullText(slice docID, sequence seq, unsigned fullTextID) {
        alloc_slice entry = getSpecialEntry(docID, seq, fullTextID);
        CollatableReader reader(entry);
        reader.beginArray();
        return reader.readString();
    }

    alloc_slice MapReduceIndex::readFullTextValue(slice docID, sequence seq, unsigned fullTextID) {
        // This data was written by emitter::emitTextTokens, below
        alloc_slice entry = getSpecialEntry(docID, seq, fullTextID);
        CollatableReader reader(entry);
        reader.beginArray();
        (void)reader.read(); // skip text
        if (reader.peekTag() == Collatable::kEndSequence)
            return alloc_slice();
        return alloc_slice(reader.read());
    }

    void MapReduceIndex::readGeoArea(slice docID, sequence seq, unsigned geoID,
                                     geohash::area &outArea,
                                     alloc_slice& outGeoJSON,
                                     alloc_slice& outValue)
    {
        alloc_slice entry = getSpecialEntry(docID, seq, geoID);
        CollatableReader reader(entry);
        reader.beginArray();
        outArea = ::forestdb::readGeoArea(reader);
        outGeoJSON = outValue = slice::null;
        if (reader.peekTag() != CollatableReader::kEndSequence) {
            if (reader.peekTag() == CollatableReader::kString)
                outGeoJSON = alloc_slice(reader.readString());
            else
                (void)reader.read();
            if (reader.peekTag() != CollatableReader::kEndSequence)
                outValue = alloc_slice(reader.read());
        }
    }


#pragma mark - MAP-REDUCE INDEXER


    // Implementation of EmitFn interface
    class emitter : public EmitFn {
    public:
        emitter()
        :_tokenizer(NULL),
         _emitCount(0)
        { }

        virtual ~emitter() {
            delete _tokenizer;
        }

        inline void emit(Collatable key, Collatable value) {
            keys.push_back(key);
            values.push_back(value);
            ++_emitCount;
        }

        virtual void operator() (Collatable key, Collatable value) {
            emit(key, value);
        }

        void emitTextTokens(slice text, Collatable value) {
            if (!_tokenizer)
                _tokenizer = new Tokenizer();
            std::unordered_map<std::string, Collatable> tokens;
            int specialKey = -1;
            for (TokenIterator i(*_tokenizer, slice(text), false); i; ++i) {
                if (specialKey < 0) {
                    // Emit the full text being indexed, and the value, under a special key.
                    specialKey = emitSpecial(text, value);
                }
                // Add the word position to the value array for this token:
                Collatable& tokValue = tokens[i.token()];
                if (tokValue.empty()) {
                    tokValue.beginArray();
                    tokValue << specialKey;
                }
                tokValue << i.wordOffset() << i.wordLength();
            }

            // Emit each token string and value array as a key:
            for (auto kv = tokens.begin(); kv != tokens.end(); ++kv) {
                Collatable collKey(kv->first);
                Collatable& collValue = kv->second;
                collValue.endArray();
                emit(collKey, collValue);
            }
        }

        static const unsigned kMaxCoveringHashes = 4;

        virtual void emit(const geohash::area& boundingBox, slice geoJSON, Collatable value) {
            Debug("emit {%g ... %g, %g ... %g}",
                  boundingBox.latitude.min, boundingBox.latitude.max,
                  boundingBox.longitude.min, boundingBox.longitude.max);
            // Emit the bbox, geoJSON, and value, under a special key:
            Collatable geoJSONColl;
            if (geoJSON.size > 0)
                geoJSONColl << geoJSON;
            unsigned specialKey = emitSpecial(boundingBox, geoJSONColl, value);
            Collatable collValue(specialKey);

            // Now emit a set of geohashes that cover the given area:
            auto hashes = boundingBox.coveringHashes();
            for (auto iHash = hashes.begin(); iHash != hashes.end(); ++iHash) {
                Debug("    hash='%s'", (const char*)(*iHash));
                Collatable collKey(*iHash);
                emit(collKey, collValue);
            }
        }

        // Saves a special key-value pair in the index that can store auxiliary data associated
        // with an emit, such as the full text or the geo-JSON. This data is read by
        // MapReduceIndex::getSpecialEntry
        template <typename KEY>
        unsigned emitSpecial(const KEY &key, Collatable value1, Collatable value2 = Collatable()) {
            Collatable collKey;
            collKey.addNull();

            Collatable collValue;
            collValue.beginArray();
            collValue << key;
            // Write value1 (or a null placeholder) then value2
            if (value1.size() > 0 || value2.size() > 0) {
                if (value1.size() > 0)
                    collValue << value1;
                else
                    collValue.addNull();
                if (value2.size() > 0)
                    collValue << value2;
            }
            collValue.endArray();

            unsigned result = _emitCount;
            emit(collKey, collValue);
            return result;
        }

        std::vector<Collatable> keys;
        std::vector<Collatable> values;

    private:
        Tokenizer* _tokenizer;
        unsigned _emitCount;
    };


    bool MapReduceIndex::updateDocInIndex(Transaction& t, const Mappable& mappable) {
        assert(t.database()->contains(*this));
        const Document& doc = mappable.document();
        if (doc.sequence() <= _lastSequenceIndexed)
            return false;
        emitter emit;
        if (!doc.deleted())
            (*_map)(mappable, emit); // Call map function!
        _lastSequenceIndexed = doc.sequence();
        if (IndexWriter(this,t).update(doc.key(), doc.sequence(), emit.keys, emit.values, _rowCount)) {
            _lastSequenceChangedAt = _lastSequenceIndexed;
            return true;
        }
        return false;
    }

    
    MapReduceIndexer::MapReduceIndexer()
    :_triggerIndex(NULL),
     _latestDbSequence(0),
     _finished(false)
    { }


    void MapReduceIndexer::addIndex(MapReduceIndex* index, Transaction* t) {
        assert(index);
        assert(t);
        _indexes.push_back(index);
        _transactions.push_back(t);
    }


    bool MapReduceIndexer::run() {
        KeyStore sourceStore = _indexes[0]->sourceStore();
        _latestDbSequence = sourceStore.lastSequence();

        // First find the minimum sequence that not all indexes have indexed yet.
        // Also start a transaction for each index:
        sequence startSequence = _latestDbSequence+1;
        for (auto idx = _indexes.begin(); idx != _indexes.end(); ++idx) {
            sequence lastSequence = (*idx)->lastSequenceIndexed();
            if (lastSequence < _latestDbSequence) {
                startSequence = std::min(startSequence, lastSequence+1);
            } else if (*idx == _triggerIndex) {
                return false; // The trigger index doesn't need to be updated, so abort
            }
            _lastSequences.push_back(lastSequence);
        }

        if (startSequence > _latestDbSequence)
            return false; // no updating needed

        // Enumerate all the documents:
        DocEnumerator::Options options = DocEnumerator::Options::kDefault;
        options.includeDeleted = true;
        for (DocEnumerator e(sourceStore, startSequence, UINT64_MAX, options); e.next(); ) {
            addDocument(*e);
        }
        _finished = true;
        return true;
    }

    MapReduceIndexer::~MapReduceIndexer() {
        unsigned i = 0;
        for (auto t = _transactions.begin(); t != _transactions.end(); ++t, ++i) {
            if (_finished)
                _indexes[i]->saveState(**t);
            else
                (*t)->abort();
            delete *t;
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
