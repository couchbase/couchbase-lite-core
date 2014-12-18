//
//  Index.cc
//  CBForest
//
//  Created by Jens Alfke on 5/14/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#include "Index.hh"
#include "Collatable.hh"
#include "varint.hh"
#include "LogInternal.hh"


namespace forestdb {

    bool KeyRange::isKeyPastEnd(slice key) const {
        return inclusiveEnd ? (key > end) : !(key < end);
    }

    Index::Index(Database* db, std::string name)
    :KeyStore(db, name)
    { }

    IndexWriter::IndexWriter(Index* index, Transaction& t)
    :KeyStoreWriter(*index, t)
    { }


    int64_t IndexWriter::removeOldRowsForDoc(slice docID) {
        int64_t rowsRemoved = 0;
        Document doc = get(docID);
        slice sequences = doc.body();
        if (sequences.size > 0) {
            uint64_t seq;
            while (ReadUVarInt(&sequences, &seq)) {
                if (!del((sequence)seq)) {
                    Warn("Index::removeOldRowsForDoc -- couldn't find seq %llu", seq);
                }
                ++rowsRemoved;
            }
        }
        return rowsRemoved;
    }

    bool IndexWriter::update(slice docID, sequence docSequence,
                             std::vector<Collatable> keys, std::vector<Collatable> values,
                             uint64_t &rowCount)
    {
        Collatable collatableDocID;
        collatableDocID << docID;

        int64_t rowsRemoved = removeOldRowsForDoc(collatableDocID);
        int64_t rowsAdded = 0;

        std::string sequences;
        auto value = values.begin();
        unsigned emitIndex = 0;
        for (auto key = keys.begin(); key != keys.end(); ++key,++value,++emitIndex) {
            Collatable realKey;
            realKey.beginArray();
            realKey << *key << collatableDocID << (int64_t)docSequence;
            if (emitIndex > 0)
                realKey << emitIndex; // This disambiguates duplicate keys emitted by the same doc
            realKey.endArray();

            if (realKey.size() <= Document::kMaxKeyLength
                    && value->size() <= Document::kMaxBodyLength) {
                sequence seq = set(realKey, slice::null, *value);

                uint8_t buf[kMaxVarintLen64];
                size_t size = PutUVarInt(buf, seq);
                sequences += std::string((char*)buf, size);
                ++rowsAdded;
            } else {
                Warn("Index key or value too long"); //FIX: Need more-official warning
            }
        }

        if (rowsRemoved==0 && rowsAdded==0)
            return false;

        set(collatableDocID, slice(sequences));
        rowCount += rowsAdded - rowsRemoved;
        return true;
    }


    alloc_slice Index::getEntry(slice docID, sequence docSequence,
                                Collatable key, unsigned emitIndex) {
        Collatable collatableDocID;
        collatableDocID << docID;
        Collatable realKey;
        realKey.beginArray();
        realKey << key << collatableDocID << (int64_t)docSequence;
        if (emitIndex > 0)
            realKey << emitIndex;
        realKey.endArray();

        Document doc = get(realKey);
        return alloc_slice(doc.body());
    }


#pragma mark - ENUMERATOR:


    // Converts an index key into the actual key used in the index db (key + docID)
    static Collatable makeRealKey(Collatable key, slice docID, bool isEnd, bool descending) {
        bool addEllipsis = (isEnd != descending);
        if (key.empty() && addEllipsis)
            return Collatable();
        Collatable realKey;
        realKey.beginArray();
        if (!key.empty()) {
            realKey << key;
            if (docID.buf)
                realKey << docID;
        }
        if (addEllipsis) {
            realKey.beginMap();
            realKey.endMap();
        }
        realKey.endArray();
        return realKey;
    }

    static DocEnumerator::Options docOptions(DocEnumerator::Options options) {
        options.limit = DocEnumerator::Options::kDefault.limit;
        options.skip = DocEnumerator::Options::kDefault.skip;
        return options;
    }

    IndexEnumerator::IndexEnumerator(Index* index,
                                     Collatable startKey, slice startKeyDocID,
                                     Collatable endKey,   slice endKeyDocID,
                                     const DocEnumerator::Options& options)
    :_index(index),
     _options(options),
     _inclusiveStart(options.inclusiveStart),
     _inclusiveEnd(options.inclusiveEnd),
     _currentKeyIndex(-1),
     _dbEnum(*_index,
             (slice)makeRealKey(startKey, startKeyDocID, false, options.descending),
             (slice)makeRealKey(endKey,   endKeyDocID,   true,  options.descending),
             docOptions(options))
    {
        Debug("IndexEnumerator(%p)", this);
        if (!_inclusiveStart)
            _startKey = (slice)startKey;
        if (!_inclusiveEnd)
            _endKey = (slice)endKey;
    }

    IndexEnumerator::IndexEnumerator(Index* index,
                                     std::vector<KeyRange> keyRanges,
                                     const DocEnumerator::Options& options)
    :_index(index),
     _options(options),
     _inclusiveStart(true),
     _inclusiveEnd(true),
     _keyRanges(keyRanges),
     _currentKeyIndex(-1),
     _dbEnum(*_index, slice::null, slice::null, docOptions(options))
    {
        Debug("IndexEnumerator(%p), key ranges:", this);
        for (auto i = _keyRanges.begin(); i != _keyRanges.end(); ++i)
            Debug("    key range: %s -- %s (%d)", i->start.dump().c_str(), i->end.dump().c_str(), i->inclusiveEnd);
        nextKeyRange();
    }

    bool IndexEnumerator::read() {
        while(true) {
            if (!_dbEnum) {
                if (_currentKeyIndex < 0)
                    return false; // at end
                else {
                    nextKeyRange();
                    if (_dbEnum.next())
                        continue;
                    else
                        return false;
                }
            }
            
            const Document& doc = _dbEnum.doc();

            // Decode the key from collatable form:
            CollatableReader reader(doc.key());
            reader.beginArray();
            _key = reader.read();

            if (!_inclusiveEnd && _key == _endKey) {
                _dbEnum.close();
                return false;
            } else if (!_inclusiveStart && _key == _startKey) {
                _dbEnum.next();
                continue;
            }

            if (_currentKeyIndex >= 0 && _keyRanges[_currentKeyIndex].isKeyPastEnd(_key)) {
                // While enumerating through _keys, advance to the next key:
                nextKeyRange();
                if (_dbEnum.next())
                    continue;
                else
                    return false;
            }

            // Subclasses can ignore rows:
            if (!this->approve(_key)) {
                _dbEnum.next();
                continue;
            }

            // OK, this is a candidate. First honor the skip and limit:
            if (_options.skip > 0) {
                --_options.skip;
                _dbEnum.next();
                continue;
            }
            if (_options.limit-- == 0) {
                _dbEnum.close();
                return false;
            }

            // Return it as the next row:
            _docID = reader.readString();
            _sequence = reader.readInt();
            _value = doc.body();
            Debug("IndexEnumerator: found key=%s",
                    forestdb::CollatableReader(_key).dump().c_str());
            return true;
        }
    }

    void IndexEnumerator::nextKeyRange() {
        if (_keyRanges.size() == 0)
            return;
        if (++_currentKeyIndex >= _keyRanges.size()) {
            _dbEnum.close();
            return;
        }

        Collatable& startKey = _keyRanges[_currentKeyIndex].start;
        Debug("IndexEnumerator: Advance to key '%s'", startKey.dump().c_str());
        if (!_dbEnum)
            _dbEnum = DocEnumerator(*_index, slice::null, slice::null, docOptions(_options));
        _dbEnum.seek(makeRealKey(startKey, slice::null, false, _options.descending));
    }

    bool IndexEnumerator::next() {
        _dbEnum.next();
        return read();
    }

}