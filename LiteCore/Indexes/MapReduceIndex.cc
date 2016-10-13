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
#include "GeoIndex.hh"
#include "Tokenizer.hh"
#include "LogInternal.hh"
#include <algorithm>

namespace litecore {
    using namespace fleece;

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
        Document state = _store.get(stateKey);
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
        Debug("MapReduceIndex<%p>: Read state (lastSeq=%lld, lastChanged=%lld, lastMapVersion='%s', indexType=%d, rowCount=%d, lastPurgeCount=%llu)",
              this, _lastSequenceIndexed, _lastSequenceChangedAt, _lastMapVersion.c_str(), _indexType, _rowCount, _lastPurgeCount);
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
        Debug("MapReduceIndex<%p>: Saved state (lastSeq=%lld, lastChanged=%lld, lastMapVersion='%s', indexType=%d, rowCount=%d, lastPurgeCount=%llu)",
              this, _lastSequenceIndexed, _lastSequenceChangedAt, _lastMapVersion.c_str(), _indexType, _rowCount, _lastPurgeCount);
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
        Debug("MapReduceIndex<%p>: Setup (indexType=%ld, mapVersion='%s')",
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
            Debug("MapReduceIndex: Erasing invalidated index");
            _store.erase();
        }
        _lastSequenceIndexed = _lastSequenceChangedAt = _lastPurgeCount = 0;
        _rowCount = 0;
        _stateReadAt = 0;
    }

    void MapReduceIndex::erase() {
        Debug("MapReduceIndex: Erasing");
        _store.erase();
        _lastSequenceIndexed = _lastSequenceChangedAt = _lastPurgeCount = 0;
        _rowCount = 0;
        _stateReadAt = 0;
    }

    alloc_slice MapReduceIndex::getSpecialEntry(slice docID, sequence seq, unsigned entryID) const
    {
        // This data was written by emitSpecial
        CollatableBuilder key;
        key.addNull();
        return getEntry(docID, seq, key, entryID);
    }

    alloc_slice MapReduceIndex::readFullText(slice docID, sequence seq, unsigned fullTextID) const {
        alloc_slice entry = getSpecialEntry(docID, seq, fullTextID);
        auto array = Value::fromTrustedData(entry)->asArray();
        return alloc_slice(array->get(0)->asString());
    }

    alloc_slice MapReduceIndex::readFullTextValue(slice docID, sequence seq, unsigned fullTextID) const {
        // This data was written by emitSpecial, as called by emitTextTokens
        alloc_slice entry = getSpecialEntry(docID, seq, fullTextID);
        auto array = Value::fromTrustedData(entry)->asArray();
        if (array->count() < 2)
            return alloc_slice();
        return alloc_slice(array->get(1)->asString());
    }

    void MapReduceIndex::readGeoArea(slice docID, sequence seq, unsigned geoID,
                                     geohash::area &outArea,
                                     alloc_slice& outGeoJSON,
                                     alloc_slice& outValue)
    {
        // Reads data written by emitter::emit(const geohash::area&,...), below
        alloc_slice entry = getSpecialEntry(docID, seq, geoID);
        Array::iterator iter(Value::fromTrustedData(entry)->asArray());
        outArea = ::litecore::readGeoArea(iter);
        outGeoJSON = outValue = nullslice;
        if (iter.count() > 0) {
            if (iter->type() == kString)
                outGeoJSON = alloc_slice(iter->asString());
            ++iter;
            if (iter)
                outValue = alloc_slice(iter->asString());
        }
    }


#pragma mark - EMITTER:


    // Collects key/value pairs being emitted
    class Emitter {
    public:

        std::vector<Collatable> keys;
        std::vector<alloc_slice> values;

        void emit(Collatable key, alloc_slice value) {
            CollatableReader keyReader(key);
            switch (keyReader.peekTag()) {
                case CollatableTypes::kFullTextKey: {
                    auto textAndLang = keyReader.readFullTextKey();
                    emitTextTokens(textAndLang.first, std::string(textAndLang.second), value);
                    break;
                }
                case CollatableTypes::kGeoJSONKey: {
                    geohash::area bbox;
                    alloc_slice geoJSON = keyReader.readGeoKey(bbox);
                    emit(bbox, geoJSON, value);
                    break;
                }
                default:
                    _emit(key, value);
                    break;
            }
        }

        void reset() {
            keys.clear();
            values.clear();
            // _tokenizer is stateless
        }

    private:

        void _emit(Collatable key, alloc_slice value) {
            keys.push_back(key);
            values.push_back(value);
        }

        void emitTextTokens(slice text, std::string languageCode, slice value) {
            if (!_tokenizer || _tokenizer->stemmer() != languageCode) {
                _tokenizer = std::unique_ptr<Tokenizer> {
                    new Tokenizer(languageCode, (languageCode == "en")) };
            }
            std::unordered_map<std::string, Encoder> tokens;
            int specialKey = -1;
            for (TokenIterator i(*_tokenizer, slice(text), false); i; ++i) {
                if (specialKey < 0) {
                    // Emit the full text being indexed, and the value, under a special key.
                    specialKey = emitSpecial(text, value);
                }
                // Add the word position to the value array for this token:
                Encoder& tokValue = tokens[i.token()];
                if (tokValue.isEmpty()) {
                    tokValue.beginArray();
                    tokValue << specialKey;
                }
                tokValue << i.wordOffset() << i.wordLength();
            }

            // Emit each token string and value array as a key:
            for (auto &kv : tokens) {
                CollatableBuilder collKey(kv.first);
                Encoder& tokValue = kv.second;
                tokValue.endArray();
                _emit(collKey, tokValue.extractOutput());
            }
        }

        static const unsigned kMaxCoveringHashes = 4;

        void emit(const geohash::area& boundingBox, slice geoJSON, slice value) {
            Debug("emit {%g ... %g, %g ... %g} --> %s",
                  boundingBox.latitude.min, boundingBox.latitude.max,
                  boundingBox.longitude.min, boundingBox.longitude.max,
                  value.hexString().c_str());
            // Emit the bbox, geoJSON, and value, under a special key:
            unsigned specialKey = emitSpecial(boundingBox, geoJSON, value);
            Encoder enc;
            enc << specialKey;
            auto specialKeyEncoded = enc.extractOutput();

            // Now emit a set of geohashes that cover the given area:
            auto hashes = boundingBox.coveringHashes();
            for (auto &hash : hashes) {
                Debug("    hash='%s'", (const char*)hash);
                CollatableBuilder collKey(hash);
                _emit(collKey, specialKeyEncoded);
            }
        }

        // Saves a special key-value pair in the index that can store auxiliary data associated
        // with an emit, such as the full text or the geo-JSON. This data is read by
        // MapReduceIndex::getSpecialEntry
        template <typename KEY>
        unsigned emitSpecial(const KEY &key, slice value1, slice value2 = nullslice) {
            CollatableBuilder collKey;
            collKey.addNull();

            Encoder encValue;
            encValue.beginArray();
            encValue << key;
            // Write value1 (or a null placeholder) then value2
            if (value1.size > 0 || value2.size > 0) {
                if (value1.size > 0)
                    encValue << value1;
                else
                    encValue.writeNull();
                if (value2.size > 0)
                    encValue << value2;
            }
            encValue.endArray();

            auto result = keys.size();
            emit(collKey, encValue.extractOutput());
            return (unsigned)result;
        }

        std::unique_ptr<Tokenizer> _tokenizer;
    };


#pragma mark - INDEX WRITER:


    // In charge of updating one view's index. Owned by a MapReduceIndexer.
    class MapReduceIndexWriter: IndexWriter {
    public:
        MapReduceIndexWriter(MapReduceIndex &idx, Transaction *t)
        :IndexWriter(idx, *t, (idx.rowCount() == 0)),
         index(idx),
         _documentType(index.documentType()),
         _transaction(t)
        { }

        MapReduceIndex & index;

        bool shouldIndexDocument(const Document& doc) const noexcept {
            return doc.sequence() > index._lastSequenceIndexed;
        }

        bool shouldIndexDocumentType(slice docType) noexcept {
            return _documentType.buf == nullptr || _documentType == docType;
        }
        
        // Writes the given rows to the index.
        bool indexDocument(slice docID,
                           sequence docSequence,
                           const std::vector<Collatable> &keys,
                           const std::vector<alloc_slice> &values)
        {
            if (docSequence <= index._lastSequenceIndexed)
                return false;
            _emitter.reset();
            for (unsigned i = 0; i < keys.size(); ++i)
                _emitter.emit(keys[i], values[i]);

            index._lastSequenceIndexed = docSequence;
            if (update(docID, docSequence, _emitter.keys, _emitter.values, index._rowCount)) {
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

    
    void MapReduceIndexer::addIndex(MapReduceIndex &index) {
        index.checkForPurge(); // has to be called before creating the transaction
        auto writer = new MapReduceIndexWriter(index, new Transaction(index.dataFile()));
        _writers.push_back(writer);
        if (index.documentType().buf)
            _docTypes.insert(index.documentType());
        else
            _allDocTypes = true;
    }


    sequence MapReduceIndexer::startingSequence() {
        _latestDbSequence = _writers[0]->index.sourceStore().lastSequence();

        // First find the minimum sequence that not all indexes have indexed yet.
        sequence startSequence = _latestDbSequence+1;
        for (auto writer : _writers) {
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

    MapReduceIndexer::~MapReduceIndexer() {
        for (auto writer : _writers) {
            delete writer;
        }
    }

    bool MapReduceIndexer::shouldMapDocIntoView(const Document &doc, unsigned viewNumber) noexcept {
        return _writers[viewNumber]->shouldIndexDocument(doc);
    }

    bool MapReduceIndexer::shouldMapDocTypeIntoView(slice docType, unsigned viewNumber) noexcept {
        return _writers[viewNumber]->shouldIndexDocumentType(docType);
    }

    void MapReduceIndexer::emitDocIntoView(slice docID,
                                           sequence docSequence,
                                           unsigned viewNumber,
                                           const std::vector<Collatable> &keys,
                                           const std::vector<alloc_slice> &values)
    {
        _writers[viewNumber]->indexDocument(docID, docSequence, keys, values);
    }

    void MapReduceIndexer::skipDoc(slice docID, sequence docSequence) {
        for (auto writer : _writers)
            writer->indexDocument(docID, docSequence, _noKeys, _noValues);
    }

    void MapReduceIndexer::skipDocInView(slice docID, sequence docSequence, unsigned viewNumber) {
        _writers[viewNumber]->indexDocument(docID, docSequence, _noKeys, _noValues);
    }

}
