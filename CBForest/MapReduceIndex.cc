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
#include <algorithm>

namespace cbforest {

    static int64_t kMinFormatVersion = 4;
    static int64_t kCurFormatVersion = 4;

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
            CollatableBuilder stateKey;
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
        CBFAssert(t.database()->contains(*this));
        _lastMapVersion = _mapVersion;

        CollatableBuilder stateKey;
        stateKey.addNull();

        CollatableBuilder state;
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
        CBFAssert(t.database()->contains(*this));
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
        CBFAssert(t.database()->contains(*this));
        KeyStore::erase(t);
        _lastSequenceIndexed = _lastSequenceChangedAt = 0;
        _rowCount = 0;
        _stateReadAt = 0;
    }

    alloc_slice MapReduceIndex::getSpecialEntry(slice docID, sequence seq, unsigned entryID)
    {
        // This data was written by emitter::emitTextTokens, below
        CollatableBuilder key;
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
        return alloc_slice(reader.readString());
    }

    void MapReduceIndex::readGeoArea(slice docID, sequence seq, unsigned geoID,
                                     geohash::area &outArea,
                                     alloc_slice& outGeoJSON,
                                     alloc_slice& outValue)
    {
        // Reads data written by emitter::emit(const geohash::area&,...), below
        alloc_slice entry = getSpecialEntry(docID, seq, geoID);
        CollatableReader reader(entry);
        reader.beginArray();
        outArea = ::cbforest::readGeoArea(reader);
        outGeoJSON = outValue = slice::null;
        if (reader.peekTag() != CollatableReader::kEndSequence) {
            if (reader.peekTag() == CollatableReader::kString)
                outGeoJSON = alloc_slice(reader.readString());
            else
                (void)reader.read();
            if (reader.peekTag() != CollatableReader::kEndSequence)
                outValue = alloc_slice(reader.readString());
        }
    }


#pragma mark - EMITTER:


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

        inline void emit(Collatable key, slice value) {
            keys.push_back(key);
            values.push_back(alloc_slice(value));
            ++_emitCount;
        }

        virtual void operator() (Collatable key, Collatable value) {
            emit(key, value);
        }

        void emitTextTokens(slice text, slice value) {
            if (!_tokenizer)
                _tokenizer = new Tokenizer();
            std::unordered_map<std::string, CollatableBuilder> tokens;
            int specialKey = -1;
            for (TokenIterator i(*_tokenizer, slice(text), false); i; ++i) {
                if (specialKey < 0) {
                    // Emit the full text being indexed, and the value, under a special key.
                    specialKey = emitSpecial(text, value);
                }
                // Add the word position to the value array for this token:
                CollatableBuilder& tokValue = tokens[i.token()];
                if (tokValue.empty()) {
                    tokValue.beginArray();
                    tokValue << specialKey;
                }
                tokValue << i.wordOffset() << i.wordLength();
            }

            // Emit each token string and value array as a key:
            for (auto kv = tokens.begin(); kv != tokens.end(); ++kv) {
                CollatableBuilder collKey(kv->first);
                CollatableBuilder& collValue = kv->second;
                collValue.endArray();
                emit(collKey, collValue);
            }
        }

        static const unsigned kMaxCoveringHashes = 4;

        virtual void emit(const geohash::area& boundingBox, slice geoJSON, slice value) {
            Debug("emit {%g ... %g, %g ... %g}",
                  boundingBox.latitude.min, boundingBox.latitude.max,
                  boundingBox.longitude.min, boundingBox.longitude.max);
            // Emit the bbox, geoJSON, and value, under a special key:
            unsigned specialKey = emitSpecial(boundingBox, geoJSON, value);
            CollatableBuilder collValue(specialKey);

            // Now emit a set of geohashes that cover the given area:
            auto hashes = boundingBox.coveringHashes();
            for (auto iHash = hashes.begin(); iHash != hashes.end(); ++iHash) {
                Debug("    hash='%s'", (const char*)(*iHash));
                CollatableBuilder collKey(*iHash);
                emit(collKey, collValue);
            }
        }

        // Saves a special key-value pair in the index that can store auxiliary data associated
        // with an emit, such as the full text or the geo-JSON. This data is read by
        // MapReduceIndex::getSpecialEntry
        template <typename KEY>
        unsigned emitSpecial(const KEY &key, slice value1, slice value2 = slice::null) {
            CollatableBuilder collKey;
            collKey.addNull();

            CollatableBuilder collValue;
            collValue.beginArray();
            collValue << key;
            // Write value1 (or a null placeholder) then value2
            if (value1.size > 0 || value2.size > 0) {
                if (value1.size > 0)
                    collValue << value1;
                else
                    collValue.addNull();
                if (value2.size > 0)
                    collValue << value2;
            }
            collValue.endArray();

            unsigned result = _emitCount;
            emit(collKey, collValue);
            return result;
        }

        void reset() {
            keys.clear();
            values.clear();
            _emitCount = 0;
            // _tokenizer is stateless
        }

        std::vector<Collatable> keys;
        std::vector<alloc_slice> values;

    private:
        Tokenizer* _tokenizer;
        unsigned _emitCount;
    };


#pragma mark - INDEX WRITER:


    class MapReduceIndexWriter: IndexWriter {
    public:
        MapReduceIndexWriter(MapReduceIndex *index, Transaction *t)
        :IndexWriter(index, *t),
         _index(index),
         _transaction(t)
        { }

        ~MapReduceIndexWriter() {
            delete _transaction;
        }

        bool shouldUpdateDocInIndex(const Document& doc) const {
            return doc.sequence() > _index->_lastSequenceIndexed;
        }

        // Calls the index's map function on 'mappable' and writes the emitted rows to the index.
        bool updateDocInIndex(const Mappable& mappable) {
            const Document& doc = mappable.document();
            _emit.reset();
            if (!doc.deleted())
                (*_index->_map)(mappable, _emit); // Call map function!
            return emitForDocument(doc.key(), doc.sequence(), _emit.keys, _emit.values);
        }

        // Writes the given rows to the index.
        bool emitDocIntoView(slice docID,
                             sequence docSequence,
                             const std::vector<Collatable> &keys,
                             const std::vector<slice> &values)
        {
            if (docSequence <= _index->_lastSequenceIndexed)
                return false;
            _emit.reset();
            for (unsigned i = 0; i < keys.size(); ++i)
                _emit.emit(keys[i], values[i]);
            return emitForDocument(docID, docSequence, _emit.keys, _emit.values);
        }

        // subroutine of updateDocInIndex and emitDocIntoView
        bool emitForDocument(slice docID,
                             sequence docSequence,
                             const std::vector<Collatable> &keys,
                             const std::vector<alloc_slice> &values)
        {
            _index->_lastSequenceIndexed = docSequence;
            if (update(docID, docSequence, keys, values, _index->_rowCount)) {
                _index->_lastSequenceChangedAt = _index->_lastSequenceIndexed;
                return true;
            }
            return false;
        }

        void saveState() {
            _index->saveState(*_transaction);
        }

        void abort() {
            _transaction->abort();
        }

        MapReduceIndex* _index;
        emitter _emit;
        Transaction *_transaction;
    };

    
#pragma mark - MAP-REDUCE INDEXER

    
    MapReduceIndexer::MapReduceIndexer()
    :_triggerIndex(NULL),
     _latestDbSequence(0),
     _finished(false)
    { }


    void MapReduceIndexer::addIndex(MapReduceIndex* index, Transaction* t) {
        CBFAssert(index);
        CBFAssert(t);
        _writers.push_back(new MapReduceIndexWriter(index, t));
    }


    KeyStore MapReduceIndexer::sourceStore() {
        return _writers[0]->_index->sourceStore();
    }


    sequence MapReduceIndexer::startingSequence() {
        _latestDbSequence = sourceStore().lastSequence();

        // First find the minimum sequence that not all indexes have indexed yet.
        sequence startSequence = _latestDbSequence+1;
        for (auto writer = _writers.begin(); writer != _writers.end(); ++writer) {
            sequence lastSequence = (*writer)->_index->lastSequenceIndexed();
            if (lastSequence < _latestDbSequence) {
                startSequence = std::min(startSequence, lastSequence+1);
            } else if ((*writer)->_index == _triggerIndex) {
                return UINT64_MAX; // The trigger index doesn't need to be updated, so abort
            }
        }
        if (startSequence > _latestDbSequence)
            startSequence = UINT64_MAX; // no updating needed
        return startSequence;
    }

    bool MapReduceIndexer::run() {
        sequence startSequence = startingSequence();
        if (startSequence > _latestDbSequence)
            return false; // no updating needed

        // Enumerate all the documents:
        DocEnumerator::Options options = DocEnumerator::Options::kDefault;
        options.includeDeleted = true;
        for (DocEnumerator e(sourceStore(), startSequence, UINT64_MAX, options); e.next(); ) {
            addDocument(*e);
        }
        finished();
        return true;
    }

    MapReduceIndexer::~MapReduceIndexer() {
        for (auto writer = _writers.begin(); writer != _writers.end(); ++writer) {
            if (_finished)
                (*writer)->saveState();
            else
                (*writer)->abort();
            delete *writer;
        }
    }

    void MapReduceIndexer::addDocument(const Document& doc) {
        Mappable mappable(doc);
        addMappable(mappable);
    }

    void MapReduceIndexer::addMappable(const Mappable& mappable) {
        for (auto writer = _writers.begin(); writer != _writers.end(); ++writer)
            if ((*writer)->shouldUpdateDocInIndex(mappable.document()))
                (*writer)->updateDocInIndex(mappable);
    }

    bool MapReduceIndexer::shouldMapDocIntoView(const Document &doc, unsigned viewNumber) {
        return _writers[viewNumber]->shouldUpdateDocInIndex(doc);
    }

    void MapReduceIndexer::emitDocIntoView(slice docID,
                                           sequence docSequence,
                                           unsigned viewNumber,
                                           const std::vector<Collatable> &keys,
                                           const std::vector<slice> &values)
    {
        _writers[viewNumber]->emitDocIntoView(docID, docSequence, keys, values);
    }

}
