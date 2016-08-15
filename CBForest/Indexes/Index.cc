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
#include "Error.hh"
#include "Fleece.hh"
#include "varint.hh"
#include "LogInternal.hh"


namespace cbforest {
    using namespace fleece;

    const slice Index::kSpecialValue("*", 1);

    bool KeyRange::isKeyPastEnd(slice key) const {
        return inclusiveEnd ? (key > end) : !(key < end);
    }

    Index::Index(KeyStore &store)
    :_store(store),
     _userCount(0)
    { }

    Index::~Index() {
        if (isBusy()) Warn("Index %p being destructed during enumeration", this);
    }

    IndexWriter::IndexWriter(Index& index, Transaction& t)
    :_index(index),
     _transaction(t)
    {
        CBFDebugAssert(&t.database() == &index._store.database());
        index.addUser();
    }

    IndexWriter::~IndexWriter() {
        _index.removeUser();
    }


    // djb2 hash function:
    static const uint32_t kInitialHash = 5381;
    static inline void addHash(uint32_t &hash, slice value) {
        for (auto i = 0; i < value.size; ++i)
            hash = ((hash << 5) + hash) + value[i];
    }

    void IndexWriter::getKeysForDoc(slice docID, std::vector<Collatable> &keys, uint32_t &hash) {
        Document doc = _index._store.get(docID);
        if (doc.body().size > 0) {
            auto keyArray = Value::fromTrustedData(doc.body())->asArray();
            Array::iterator iter(keyArray);
            hash = (uint32_t)iter->asUnsigned();
            ++iter;
            keys.reserve(iter.count());
            for (; iter; ++iter) {
                keys.push_back( Collatable::withData(iter->asString()) );
            }
        } else {
            hash = kInitialHash;
        }
    }

    void IndexWriter::setKeysForDoc(slice docID, const std::vector<Collatable> &keys, uint32_t hash) {
        if (keys.size() > 0) {
            Encoder enc;
            enc.beginArray();
            enc.writeUInt(hash);
            for (auto &key : keys)
                enc.writeData(key);
            enc.endArray();
            _index._store.set(docID, enc.extractOutput(), _transaction);
        } else {
            _index._store.del(docID, _transaction);
        }
    }

    bool IndexWriter::update(slice docID, sequence docSequence,
                             const std::vector<Collatable> &keys,
                             const std::vector<alloc_slice> &values,
                             uint64_t &rowCount)
    {
        CollatableBuilder collatableDocID;
        collatableDocID << docID;

        // Metadata of emitted rows contains doc sequence as varint:
        uint8_t metaBuf[10];
        slice meta(metaBuf, PutUVarInt(metaBuf, docSequence));

        // Get the previously emitted keys:
        std::vector<Collatable> oldStoredKeys, newStoredKeys;
        uint32_t oldStoredHash;
        getKeysForDoc(collatableDocID, oldStoredKeys, oldStoredHash);

        // Compute a hash of the values and see whether it's the same as the previous values' hash:
        uint32_t newStoredHash = kInitialHash;
        for (auto &value : values) {
            if (value == Index::kSpecialValue) {
                // kSpecialValue is placeholder for entire doc, and always considered changed.
                oldStoredHash = newStoredHash - 1; // force comparison to fail
                break;
            }
            addHash(newStoredHash, value);
        }
        bool valuesMightBeUnchanged = (newStoredHash == oldStoredHash);

        bool keysChanged = false;
        int64_t rowsRemoved = 0, rowsAdded = 0;

        auto value = values.begin();
        unsigned emitIndex = 0;
        auto oldKey = oldStoredKeys.begin();
        for (auto key = keys.begin(); key != keys.end(); ++key,++value,++emitIndex) {
            // Create a key for the index db by combining the emitted key, doc ID, and emit#:
            CollatableBuilder realKey;
            realKey.beginArray() << *key << collatableDocID;
            if (emitIndex > 0)
                realKey << emitIndex;
            realKey.endArray();

            // Is this a key that was previously emitted last time we indexed this document?
            if (keysChanged || oldKey == oldStoredKeys.end() || !(*oldKey == *key)) {
                // no; note that the set of keys is different
                keysChanged = true;
            } else {
                // yes
                ++oldKey;
                if (valuesMightBeUnchanged) {
                    // read the old row so we can compare the value too:
                    Document oldRow = _index._store.get(realKey);
                    if (oldRow.exists()) {
                        if (oldRow.body() == *value) {
                            Log("Old k/v pair (%s, %s) unchanged",
                                key->toJSON().c_str(), ((std::string)*value).c_str());
                            continue;  // Value is unchanged, so this is a no-op; skip to next key!
                        }
                    } else {
                        Warn("Old emitted k/v pair unexpectedly missing");
                    }
                }
                ++rowsRemoved;  // more like "overwritten"
            }

            // Store the key & value:
            Log("**** Index: realKey = %s  value = %s",
                realKey.toJSON().c_str(), (*value).hexString().c_str());
            _index._store.set(realKey, meta, *value, _transaction);
            newStoredKeys.push_back(*key);
            ++rowsAdded;
        }

        // If there are any old keys that weren't emitted this time, we need to delete those rows:
        for (; oldKey != oldStoredKeys.end(); ++oldKey) {
            CollatableBuilder realKey;
            realKey.beginArray() << *oldKey << collatableDocID;
            auto oldEmitIndex = oldKey - oldStoredKeys.begin();
            if (oldEmitIndex > 0)
                realKey << oldEmitIndex;
            realKey.endArray();
            bool deleted = _index._store.del(realKey, _transaction);
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
                                Collatable key, unsigned emitIndex) const {
        CollatableBuilder collatableDocID;
        collatableDocID << docID;

        // realKey matches the key generated in update(), above
        CollatableBuilder realKey;
        realKey.beginArray();
        realKey << key << collatableDocID;
        if (emitIndex > 0)
            realKey << emitIndex;
        realKey.endArray();

        Log("**** getEntry: realKey = %s", realKey.toJSON().c_str());
        Document doc = _store.get(realKey);
        CBFAssert(doc.exists());
        return alloc_slice(doc.body());
    }


#pragma mark - ENUMERATOR:


    // Converts an index key into the actual key used in the index db (key + docID)
    static Collatable makeRealKey(Collatable key, slice docID, bool isEnd, bool descending) {
        bool addEllipsis = (isEnd != descending);
        if (key.empty() && addEllipsis)
            return Collatable();
        CollatableBuilder realKey;
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
        options.includeDeleted = false;
        options.contentOptions = kDefaultContent; // read() method needs the doc bodies
        return options;
    }

    IndexEnumerator::IndexEnumerator(Index& index,
                                     Collatable startKey, slice startKeyDocID,
                                     Collatable endKey,   slice endKeyDocID,
                                     const DocEnumerator::Options& options)
    :_index(index),
     _options(options),
     _inclusiveStart(options.inclusiveStart),
     _inclusiveEnd(options.inclusiveEnd),
     _dbEnum(_index._store,
             (slice)makeRealKey(startKey, startKeyDocID, false, options.descending),
             (slice)makeRealKey(endKey,   endKeyDocID,   true,  options.descending),
             docOptions(options))
    {
        Debug("IndexEnumerator(%p)", this);
        index.addUser();
        if (!_inclusiveStart)
            _startKey = (slice)startKey;
        if (!_inclusiveEnd)
            _endKey = (slice)endKey;
    }

    IndexEnumerator::IndexEnumerator(Index& index,
                                     std::vector<KeyRange> keyRanges,
                                     const DocEnumerator::Options& options)
    :_index(index),
     _options(options),
     _inclusiveStart(true),
     _inclusiveEnd(true),
     _keyRanges(keyRanges),
     _currentKeyIndex(0),
     _dbEnum(enumeratorForIndex(0))
    {
#if DEBUG
        Debug("IndexEnumerator(%p), key ranges:", this);
        for (auto &r : _keyRanges)
            Debug("    key range: %s -- %s (%d)", r.start.toJSON().c_str(), r.end.toJSON().c_str(), r.inclusiveEnd);
#endif
        index.addUser();
        if (_keyRanges.size() == 0)
            _dbEnum.close();

    }

    bool IndexEnumerator::read() {
        while(true) {
            if (!_dbEnum) {
                if (_currentKeyIndex < 0)
                    return false; // at end
                else {
                    if (!nextKeyRange())
                        return false; // out of ranges
                    _dbEnum.next();
                    continue;
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

            _docID = keyReader.readString();
            GetUVarInt(doc.meta(), &_sequence);
            _value = doc.body();

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
            Debug("IndexEnumerator: found key=%s",
                    cbforest::CollatableReader(_key).toJSON().c_str());
            return true;
        }
    }

    bool IndexEnumerator::nextKeyRange() {
        if (++_currentKeyIndex >= _keyRanges.size()) {
            _dbEnum.close();
            return false;
        }
        _dbEnum = enumeratorForIndex(_currentKeyIndex);
        return true;
    }

    DocEnumerator IndexEnumerator::enumeratorForIndex(int i) {
        if (i >= _keyRanges.size()) {
            return DocEnumerator(_index._store);
        }
        Collatable& startKey = _keyRanges[i].start;
        Collatable& endKey = _keyRanges[i].end;
        Debug("IndexEnumerator: Advance to key range #%d, '%s'", i, startKey.toJSON().c_str());
        return DocEnumerator(_index._store,
                             makeRealKey(startKey, slice::null, false, _options.descending),
                             makeRealKey(endKey,   slice::null, true,  _options.descending),
                             docOptions(_options));
    }

    bool IndexEnumerator::next() {
        _dbEnum.next();
        return read();
    }

}
