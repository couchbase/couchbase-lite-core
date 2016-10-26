//
//  Index.cc
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 5/14/14.
//  Copyright (c) 2014-2016 Couchbase. All rights reserved.
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
#include "Logging.hh"


namespace litecore {
    using namespace fleece;

    LogDomain IndexLog("Index");

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

    IndexWriter::IndexWriter(Index& index, Transaction& t, bool wasEmpty)
    :_index(index),
     _transaction(t),
     _wasEmpty(wasEmpty)
    {
        DebugAssert(&t.dataFile() == &index._store.dataFile());
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

    void IndexWriter::getKeysForDoc(slice recordID, std::vector<Collatable> &keys, uint32_t &hash) {
        if (!_wasEmpty) {
            Record rec = _index._store.get(recordID);
            if (rec.body().size > 0) {
                auto keyArray = Value::fromTrustedData(rec.body())->asArray();
                Array::iterator iter(keyArray);
                hash = (uint32_t)iter->asUnsigned();
                ++iter;
                keys.reserve(iter.count());
                for (; iter; ++iter) {
                    keys.push_back( Collatable::withData(iter->asData()) );
                }
                return;
            }
        }
        hash = kInitialHash;
    }

    void IndexWriter::setKeysForDoc(slice recordID, const std::vector<Collatable> &keys, uint32_t hash) {
        if (keys.size() > 0) {
            _encoder.reset();
            _encoder.beginArray();
            _encoder.writeUInt(hash);
            for (auto &key : keys)
                _encoder.writeData(key);
            _encoder.endArray();
            _index._store.set(recordID, _encoder.extractOutput(), _transaction);
        } else if (!_wasEmpty) {
            _index._store.del(recordID, _transaction);
        }
    }

    bool IndexWriter::update(slice recordID, sequence recordSequence,
                             const std::vector<Collatable> &keys,
                             const std::vector<alloc_slice> &values,
                             uint64_t &rowCount)
    {
        if (_wasEmpty && keys.empty())
            return false;

        CollatableBuilder collatableDocID;
        collatableDocID << recordID;

        // Metadata of emitted rows contains rec sequence as varint:
        uint8_t metaBuf[10];
        slice meta(metaBuf, PutUVarInt(metaBuf, recordSequence));

        // Get the previously emitted keys:
        std::vector<Collatable> oldStoredKeys, newStoredKeys;
        uint32_t oldStoredHash;
        getKeysForDoc(collatableDocID, oldStoredKeys, oldStoredHash);

        // Compute a hash of the values and see whether it's the same as the previous values' hash:
        uint32_t newStoredHash = kInitialHash;
        for (auto &value : values) {
            if (value == Index::kSpecialValue) {
                // kSpecialValue is placeholder for entire rec, and always considered changed.
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
            // Create a key for the index db by combining the emitted key, rec ID, and emit#:
            _realKey.reset();
            _realKey.beginArray() << *key << collatableDocID;
            if (emitIndex > 0)
                _realKey << emitIndex;
            _realKey.endArray();

            // Is this a key that was previously emitted last time we indexed this record?
            if (keysChanged || oldKey == oldStoredKeys.end() || !(*oldKey == *key)) {
                // no; note that the set of keys is different
                keysChanged = true;
            } else {
                // yes
                ++oldKey;
                if (valuesMightBeUnchanged) {
                    // read the old row so we can compare the value too:
                    Record oldRow = _index._store.get(_realKey);
                    if (oldRow.exists()) {
                        if (oldRow.body() == *value) {
                            LogTo(IndexLog, "Old k/v pair (%s, %s) unchanged",
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
            LogTo(IndexLog, "**** Index: realKey = %s  value = %s",
                _realKey.toJSON().c_str(), (*value).hexString().c_str());
            _index._store.set(_realKey, meta, *value, _transaction);
            newStoredKeys.push_back(*key);
            ++rowsAdded;
        }

        // If there are any old keys that weren't emitted this time, we need to delete those rows:
        for (; oldKey != oldStoredKeys.end(); ++oldKey) {
            _realKey.reset();
            _realKey.beginArray() << *oldKey << collatableDocID;
            auto oldEmitIndex = oldKey - oldStoredKeys.begin();
            if (oldEmitIndex > 0)
                _realKey << oldEmitIndex;
            _realKey.endArray();
            bool deleted = _index._store.del(_realKey, _transaction);
            if (!deleted) {
                Warn("Failed to delete old emitted k/v pair");
            }
            ++rowsRemoved;
            keysChanged = true;
        }

        // Store the keys that were emitted for this rec, and the hash of the values:
        if (keysChanged)
            setKeysForDoc(collatableDocID, newStoredKeys, newStoredHash);

        if (rowsRemoved==0 && rowsAdded==0)
            return false;

        rowCount += rowsAdded - rowsRemoved;
        return true;
    }


    alloc_slice Index::getEntry(slice recordID, sequence recordSequence,
                                Collatable key, unsigned emitIndex) const {
        CollatableBuilder collatableDocID;
        collatableDocID << recordID;

        // realKey matches the key generated in update(), above
        CollatableBuilder realKey;
        realKey.beginArray();
        realKey << key << collatableDocID;
        if (emitIndex > 0)
            realKey << emitIndex;
        realKey.endArray();

        LogTo(IndexLog, "**** getEntry: realKey = %s", realKey.toJSON().c_str());
        Record rec = _store.get(realKey);
        Assert(rec.exists());
        return alloc_slice(rec.body());
    }


#pragma mark - ENUMERATOR:


    // Converts an index key into the actual key used in the index db (key + recordID)
    static Collatable makeRealKey(Collatable key, slice recordID, bool isEnd, bool descending) {
        bool addEllipsis = (isEnd != descending);
        if (key.empty() && addEllipsis)
            return Collatable();
        CollatableBuilder realKey;
        realKey.beginArray();
        if (!key.empty()) {
            realKey << key;
            if (recordID.buf)
                realKey << recordID;
        }
        if (addEllipsis) {
            realKey.beginMap();
            realKey.endMap();
        }
        realKey.endArray();
        return realKey;
    }

    static RecordEnumerator::Options recordOptions(IndexEnumerator::Options options) {
        options.limit = UINT_MAX;
        options.skip = 0;
        options.includeDeleted = false;
        options.contentOptions = kDefaultContent; // read() method needs the rec bodies
        return options;
    }

    IndexEnumerator::IndexEnumerator(Index& index,
                                     Collatable startKey, slice startKeyDocID,
                                     Collatable endKey,   slice endKeyDocID,
                                     const Options& options)
    :_index(index),
     _options(options),
     _inclusiveStart(options.inclusiveStart),
     _inclusiveEnd(options.inclusiveEnd),
     _dbEnum(_index._store,
             (slice)makeRealKey(startKey, startKeyDocID, false, options.descending),
             (slice)makeRealKey(endKey,   endKeyDocID,   true,  options.descending),
             recordOptions(options))
    {
        LogToAt(IndexLog, Debug, "IndexEnumerator(%p)", this);
        index.addUser();
        if (!_inclusiveStart)
            _startKey = (slice)startKey;
        if (!_inclusiveEnd)
            _endKey = (slice)endKey;
    }

    IndexEnumerator::IndexEnumerator(Index& index,
                                     std::vector<KeyRange> keyRanges,
                                     const Options& options)
    :_index(index),
     _options(options),
     _inclusiveStart(true),
     _inclusiveEnd(true),
     _keyRanges(keyRanges),
     _currentKeyIndex(0),
     _dbEnum(enumeratorForIndex(0))
    {
#if DEBUG
        LogToAt(IndexLog, Debug, "IndexEnumerator(%p), key ranges:", this);
        for (auto &r : _keyRanges)
            LogToAt(IndexLog, Debug, "    key range: %s -- %s (%d)", r.start.toJSON().c_str(), r.end.toJSON().c_str(), r.inclusiveEnd);
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
            
            const Record& rec = _dbEnum.record();

            // Decode the key from collatable form:
            CollatableReader keyReader(rec.key());
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

            _recordID = keyReader.readString();
            GetUVarInt(rec.meta(), &_sequence);
            _value = rec.body();

            // Subclasses can ignore rows:
            if (!this->approve(_key)) {
                _dbEnum.next();
                continue;
            }

            // If reducing/grouping, either accumulate this row, or generate a reduced row:
            if (_options.reduce) {
                if (accumulateRow()) {
                    _dbEnum.next();
                    continue;
                }
                createReducedRow();
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
            LogToAt(IndexLog, Debug, "IndexEnumerator: found key=%s",
                    litecore::CollatableReader(_key).toJSON().c_str());
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

    RecordEnumerator IndexEnumerator::enumeratorForIndex(int i) {
        if (i >= _keyRanges.size()) {
            return RecordEnumerator(_index._store);
        }
        Collatable& startKey = _keyRanges[i].start;
        Collatable& endKey = _keyRanges[i].end;
        LogToAt(IndexLog, Debug, "IndexEnumerator: Advance to key range #%d, '%s'", i, startKey.toJSON().c_str());
        return RecordEnumerator(_index._store,
                             makeRealKey(startKey, nullslice, false, _options.descending),
                             makeRealKey(endKey,   nullslice, true,  _options.descending),
                             recordOptions(_options));
    }

    bool IndexEnumerator::next() {
        _dbEnum.next();
        if (read()) {
            return true;
        } else if (_options.reduce && createReducedRow()) {
            return true;
        } else {
            return false;
        }
    }


#pragma mark - REDUCE:


    // Accumulates the current row into the reduce, if appropriate; else returns false.
    bool IndexEnumerator::accumulateRow() {
        if (_options.groupLevel > 0) {
            if (!_reducing) {
                // First row: find the key we're grouping on:
                computeGroupedKey();
            } else if (_key.size < _groupedKey.size ||
                        0 != memcmp(_key.buf, _groupedKey.buf, _groupedKey.size)) {
                // If the current key doesn't have the current grouped key prefix, don't accumulate:
                return false;
            }
        }
        // Feed the row into the reduce function:
        (*_options.reduce)(_key, _value);
        _reducing = true;
        return true;
    }


    // Gets the accumulated reduced value from the reducer and stores it in _value.
    // Stores the current grouped key [prefix] into _key.
    // If not at the end of the iteration, starts a new reduce with the current row.
    bool IndexEnumerator::createReducedRow() {
        if (!_reducing)
            return false;
        // Compute the reduced key/value of the preceding rows:
        _reducedKey = _groupedKey;
        if (_reducedKey.size == 0) {
            uint8_t defaultKey[1] = {CollatableTypes::kNull};
            _reducedKey = slice(defaultKey, 1);
        } else if (_reducedKey[0] == Collatable::kArray) {
            uint8_t suffix[1] = {CollatableTypes::kEndSequence};
            _reducedKey.append(slice(suffix, 1));
        }
        slice reducedValue = _options.reduce->reducedValue();
        _reducing = false;

        if (_dbEnum && _options.groupLevel > 0) {
            // Get the new grouped (prefix) key:
            computeGroupedKey();

            // Start a new reduce from the current row:
            (*_options.reduce)(_key, _value);
            _reducing = true;
        }

        // Expose the reduced key & value:
        _key = _reducedKey;
        _value = reducedValue;
        return true;
    }


    // Set _groupedKey equal to the key or key-prefix that's being grouped on.
    void IndexEnumerator::computeGroupedKey() {
        CollatableReader keyReader = key();
        if (keyReader.peekTag() == CollatableTypes::kArray) {
            keyReader.skipTag();
            for (unsigned level = 0; level < _options.groupLevel; ++level) {
                if (keyReader.atEnd())
                    break;
                (void)keyReader.read();
            }
            _groupedKey = alloc_slice(_key.buf, keyReader.data().buf);
        } else {
            _groupedKey = _key;
        }
    }

}
