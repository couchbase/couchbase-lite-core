//
//  Index.cc
//  CBForest
//
//  Created by Jens Alfke on 5/14/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#include "Index.hh"
#include "Collatable.hh"
#include "varint.h"

namespace forestdb {

    Index::Index(std::string path, Database::openFlags flags, const Database::config& config)
    :Database(path, flags, config)
    { }

    bool Index::removeOldRowsForDoc(Transaction& transaction, slice docID) {
        Document doc = get(docID);
        ::slice sequences = doc.body();
        if (sequences.size == 0)
            return false;
        uint64_t seq;
        while (::ReadUVarInt(&sequences, &seq))
            transaction.del((sequence)seq);
        return true;
    }

    bool Index::update(IndexTransaction& transaction,
                       slice docID, sequence docSequence,
                       std::vector<Collatable> keys, std::vector<Collatable> values)
    {
        Collatable collatableDocID;
        collatableDocID << docID;

        bool hadRows = removeOldRowsForDoc(transaction, collatableDocID);

        std::string sequences;
        auto value = values.begin();
        for (auto key = keys.begin(); key != keys.end(); ++key,++value) {
            Collatable realKey;
            realKey.beginArray();
            realKey << *key << collatableDocID << (int64_t)docSequence;
            realKey.endArray();

            sequence seq = transaction.set(realKey, slice::null, *value);

            uint8_t buf[kMaxVarintLen64];
            size_t size = PutUVarInt(buf, seq);
            sequences += std::string((char*)buf, size);
        }

        if (!hadRows && sequences.size()==0)
            return false;

        transaction.set(collatableDocID, slice(sequences));
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
            if (docID)
                realKey << docID;
        }
        if (addEllipsis) {
            realKey.beginMap();
            realKey.endMap();
        }
        realKey.endArray();
        return realKey;
    }

    IndexEnumerator::IndexEnumerator(Index& index,
                                     Collatable startKey, slice startKeyDocID,
                                     Collatable endKey,   slice endKeyDocID,
                                     const Database::enumerationOptions* options)
    :_index(index),
     _endKey(makeRealKey(endKey, endKeyDocID, (!options || !options->descending))),
     _currentKeyIndex(-1),
     _dbEnum(_index.Database::enumerate((slice)makeRealKey(startKey, startKeyDocID,
                                                           (options && options->descending)),
                                         _endKey,
                                         options))
    {
        read();
    }

    IndexEnumerator::IndexEnumerator(Index& index,
                                     std::vector<Collatable> keys,
                                     const Database::enumerationOptions* options)
    :_index(index),
     _keys(keys),
     _currentKeyIndex(-1),
     _dbEnum(_index.Database::enumerate(slice::null, slice::null, options))
    {
        std::sort(_keys.begin(), _keys.end());
        nextKey();
        read();
    }

    bool IndexEnumerator::read() {
        while(true) {
            if (!_dbEnum)
                return false; // at end
            const Document& doc = _dbEnum.doc();

            // Decode the key from collatable form:
            CollatableReader reader(doc.key());
            reader.beginArray();
            _key = reader.read();
            _docID = reader.readString();
            _sequence = reader.readInt();
            _value = Collatable(doc.body());
            fprintf(stderr, "IndexEnumerator: key=%s\n",
                    forestdb::CollatableReader(_key).dump().c_str());

            if (_currentKeyIndex < 0 || _key.equal(_keys[_currentKeyIndex]))
                return true; // normal enumeration
            // While enumerating through _keys, advance to the next key:
            if (!nextKey())
                return false;
        }
    }

    bool IndexEnumerator::nextKey() {
        if (_keys.size() == 0)
            return false;
        if (++_currentKeyIndex >= _keys.size()) {
            _dbEnum.close();
            return false;
        }
        fprintf(stderr, "IndexEnumerator: Advance to key '%s'\n", _keys[_currentKeyIndex].dump().c_str());
        return _dbEnum.seek(makeRealKey(_keys[_currentKeyIndex], slice::null, false));
    }

    bool IndexEnumerator::next() {
        if (!_dbEnum.next())
            return false;
        return read();
    }

}