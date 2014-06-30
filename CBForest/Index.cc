//
//  Index.cc
//  CBForest
//
//  Created by Jens Alfke on 5/14/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#include "Index.hh"
#include "Collatable.hh"
#include "varint.hh"

// Logging:
#if 0
#define Log(FMT, ARGS...) fprintf(stderr, FMT, ##ARGS)
#else
#define Log(FMT, ARGS...) {}
#endif


namespace forestdb {

    Index::Index(std::string path, Database::openFlags flags, const Database::config& config)
    :Database(path, flags, config)
    { }

    bool IndexTransaction::removeOldRowsForDoc(slice docID) {
        Document doc = get(docID);
        slice sequences = doc.body();
        if (sequences.size == 0)
            return false;
        uint64_t seq;
        while (ReadUVarInt(&sequences, &seq))
            del((sequence)seq);
        return true;
    }

    bool IndexTransaction::update(slice docID, sequence docSequence,
                       std::vector<Collatable> keys, std::vector<Collatable> values)
    {
        Collatable collatableDocID;
        collatableDocID << docID;

        bool hadRows = removeOldRowsForDoc(collatableDocID);

        std::string sequences;
        auto value = values.begin();
        for (auto key = keys.begin(); key != keys.end(); ++key,++value) {
            Collatable realKey;
            realKey.beginArray();
            realKey << *key << collatableDocID << (int64_t)docSequence;
            realKey.endArray();

            sequence seq = set(realKey, slice::null, *value);

            uint8_t buf[kMaxVarintLen64];
            size_t size = PutUVarInt(buf, seq);
            sequences += std::string((char*)buf, size);
        }

        if (!hadRows && sequences.size()==0)
            return false;

        set(collatableDocID, slice(sequences));
        return true;
    }


#pragma mark - ENUMERATOR:


    // Converts an index key into the actual key used in the index db (key + docID)
    static Collatable makeRealKey(Collatable key, slice docID, bool addEllipsis) {
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

    IndexEnumerator::IndexEnumerator(Index& index,
                                     Collatable startKey, slice startKeyDocID,
                                     Collatable endKey,   slice endKeyDocID,
                                     const DocEnumerator::Options& options)
    :_index(index),
     _options(options),
     _inclusiveEnd(options.inclusiveEnd),
     _currentKeyIndex(-1),
     _dbEnum(&_index,
             (slice)makeRealKey(startKey, startKeyDocID, false),
             (slice)makeRealKey(endKey, endKeyDocID, true),
             docOptions(options))
    {
        Log("IndexEnumerator(%p)\n", this);
        if (!_inclusiveEnd)
            _endKey = (slice)endKey;
        read();
    }

    IndexEnumerator::IndexEnumerator(Index& index,
                                     std::vector<Collatable> keys,
                                     const DocEnumerator::Options& options)
    :_index(index),
     _options(options),
     _inclusiveEnd(true),
     _keys(keys),
     _currentKeyIndex(-1),
     _dbEnum(&_index, slice::null, slice::null, docOptions(options))
    {
        Log("IndexEnumerator(%p)\n", this);
        nextKey();
        read();
    }

    bool IndexEnumerator::read() {
        while(true) {
            if (!_dbEnum) {
                if (_currentKeyIndex < 0)
                    return false; // at end
                else if (nextKey())
                    continue;
                else
                    return false;
            }
            
            const Document& doc = _dbEnum.doc();

            // Decode the key from collatable form:
            CollatableReader reader(doc.key());
            reader.beginArray();
            _key = reader.read();

            if (!_inclusiveEnd && _key == _endKey) {
                _dbEnum.close();
                return false;
            }

            if (_currentKeyIndex >= 0 && _key != _keys[_currentKeyIndex]) {
                // While enumerating through _keys, advance to the next key:
                if (nextKey())
                    continue;
                else
                    return false;
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
            Log("IndexEnumerator: key=%s\n",
                    forestdb::CollatableReader(_key).dump().c_str());
            return true;
        }
    }

    bool IndexEnumerator::nextKey() {
        if (_keys.size() == 0)
            return false;
        if (++_currentKeyIndex >= _keys.size()) {
            _dbEnum.close();
            return false;
        }

        if (_currentKeyIndex > 0 && !(_keys[_currentKeyIndex-1] < _keys[_currentKeyIndex])) {
            _dbEnum = DocEnumerator(&_index, slice::null, slice::null, _options);
        }

        Log("IndexEnumerator: Advance to key '%s'\n", _keys[_currentKeyIndex].dump().c_str());
        return _dbEnum.seek(makeRealKey(_keys[_currentKeyIndex], slice::null, false));
    }

    bool IndexEnumerator::next() {
        _dbEnum.next();
        return read();
    }

}