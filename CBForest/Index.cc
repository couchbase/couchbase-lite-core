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
#include "hash_functions.h" // from forestdb source


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


    // djb2 hash function:
    static const uint32_t kInitialHash = 5381;
    static inline void addHash(uint32_t &hash, slice value) {
        for (auto i = 0; i < value.size; ++i)
            hash = ((hash << 5) + hash) + value[i];
    }

    void IndexWriter::getKeysForDoc(slice docID, std::vector<Collatable> &keys, uint32_t &hash) {
        Document doc = get(docID);
        if (doc.body().size > 0) {
            CollatableReader reader(doc.body());
            hash = (uint32_t)reader.readInt();
            while (!reader.atEnd()) {
                keys.push_back( Collatable(reader.read(), true) );
            }
        } else {
            hash = kInitialHash;
        }
    }

    void IndexWriter::setKeysForDoc(slice docID, const std::vector<Collatable> &keys, uint32_t hash) {
        if (keys.size() > 0) {
            Collatable writer;
            writer << hash;
            for (auto i=keys.begin(); i != keys.end(); ++i)
                writer << *i;
            set(docID, writer);
        } else {
            del(docID);
        }
    }

    bool IndexWriter::update(slice docID, sequence docSequence,
                             std::vector<Collatable> keys, std::vector<Collatable> values,
                             uint64_t &rowCount)
    {
        Collatable collatableDocID;
        collatableDocID << docID;

        // Get the previously emitted keys:
        std::vector<Collatable> oldStoredKeys, newStoredKeys;
        uint32_t oldStoredHash;
        getKeysForDoc(collatableDocID, oldStoredKeys, oldStoredHash);

        // Compute a hash of the values and see whether it's the same as the previous values' hash:
        uint32_t newStoredHash = kInitialHash;
        for (auto value = values.begin(); value != values.end(); ++value) {
            if (((slice)*value)[0] == Collatable::kSpecial) {
                // kSpecial is placeholder for entire doc, and always considered to have changed.
                oldStoredHash = newStoredHash - 1; // force comparison to fail
                break;
            }
            addHash(newStoredHash, *value);
        }
        bool valuesMightBeUnchanged = (newStoredHash == oldStoredHash);

        bool keysChanged = false;
        int64_t rowsRemoved = 0, rowsAdded = 0;

        auto value = values.begin();
        unsigned emitIndex = 0;
        auto oldKey = oldStoredKeys.begin();
        for (auto key = keys.begin(); key != keys.end(); ++key,++value,++emitIndex) {
            // Create a key for the index db by combining the emitted key and doc ID:
            Collatable realKey;
            realKey.beginArray() << *key << collatableDocID;
            if (emitIndex > 0)
                realKey << emitIndex;
            realKey.endArray();
            if (realKey.size() > Document::kMaxKeyLength
                    || value->size() > Document::kMaxBodyLength) {
                Warn("Index key or value too long"); //FIX: Need more-official warning
                continue;
            }

            // Is this a key that was previously emitted last time we indexed this document?
            if (keysChanged || oldKey == oldStoredKeys.end() || !(*oldKey == *key)) {
                // no; note that the set of keys is different
                keysChanged = true;
            } else {
                // yes
                ++oldKey;
                if (valuesMightBeUnchanged) {
                    // read the old row so we can compare the value too:
                    Document oldRow = get(realKey);
                    if (oldRow.exists()) {
                        CollatableReader body(oldRow.body());
                        body.beginArray();
                        (void)body.readInt(); // old doc sequence
                        slice oldValue = slice::null;
                        if (body.peekTag() != Collatable::kEndSequence)
                            oldValue = body.read();

                        if (oldValue == (slice)*value) {
                            Log("Old k/v pair (%s, %s) unchanged",
                                  key->dump().c_str(), value->dump().c_str());
                            continue;  // Value is unchanged, so this is a no-op; skip to next key!
                        }
                    } else {
                        Warn("Old emitted k/v pair unexpectedly missing");
                    }
                }
                ++rowsRemoved;  // more like "overwritten"
            }

            // Store the key & value:
            Collatable realValue;
            realValue.beginArray() << docSequence << *value;
            realValue.endArray();
            set(realKey, slice::null, realValue);
            newStoredKeys.push_back(*key);
            ++rowsAdded;
        }

        // If there are any old keys that weren't emitted this time, we need to delete those rows:
        for (; oldKey != oldStoredKeys.end(); ++oldKey) {
            Collatable realKey;
            realKey.beginArray() << *oldKey << collatableDocID;
            auto oldEmitIndex = oldKey - oldStoredKeys.begin();
            if (oldEmitIndex > 0)
                realKey << oldEmitIndex;
            realKey.endArray();
            bool deleted = del(realKey);
            if (!deleted) {
                Warn("Failed to delete old emitted k/v pair");
            }
            ++rowsRemoved;
            keysChanged = true;
        }

        // Store the keys that were emitted for this doc, and the hash of the values:
        if (keysChanged)
            setKeysForDoc(collatableDocID, newStoredKeys, newStoredHash);

        if (rowsRemoved==0 && rowsAdded==0)
            return false;

        rowCount += rowsAdded - rowsRemoved;
        return true;
    }


    alloc_slice Index::getEntry(slice docID, sequence docSequence,
                                Collatable key, unsigned emitIndex) {
        Collatable collatableDocID;
        collatableDocID << docID;
        Collatable realKey;
        realKey.beginArray();
        realKey << key << collatableDocID;
        if (emitIndex > 0)
            realKey << emitIndex;
        realKey.endArray();

        Document doc = get(realKey);

        CollatableReader realValue(doc.body());
        realValue.beginArray();
        (void)realValue.readInt(); // doc sequence
        if (realValue.peekTag() == Collatable::kEndSequence)
            return alloc_slice();
        return alloc_slice(realValue.read());
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
            CollatableReader keyReader(doc.key());
            keyReader.beginArray();
            _key = keyReader.read();

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
            _docID = keyReader.readString();

            CollatableReader valueReader(doc.body());
            valueReader.beginArray();
            _sequence = valueReader.readInt();
            if (valueReader.peekTag() == Collatable::kEndSequence)
                _value = slice::null;
            else
                _value = valueReader.read();
            Debug("IndexEnumerator: found key=%s",
                    forestdb::CollatableReader(_key).dump().c_str());
            return true;
        }
    }

    void IndexEnumerator::getTextToken(std::string &token,
                                       size_t &wordStart, size_t &wordLength,
                                       unsigned &fullTextID)
    {
        CollatableReader reader(value());
        reader.beginArray();
        fullTextID = (unsigned)reader.readInt();
        wordStart = (size_t)reader.readDouble();
        wordLength = (size_t)reader.readDouble();
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