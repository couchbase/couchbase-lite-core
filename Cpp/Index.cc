//
//  Index.cc
//  CBForest
//
//  Created by Jens Alfke on 5/14/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#include "Index.h"
#include "Collatable.h"
#include "varint.h"

namespace forestdb {

    Index::Index(std::string path, Database::openFlags flags, const Database::config& config)
    :Database(path, flags, config)
    { }

    bool Index::removeOldRowsForDoc(Transaction& transaction, slice docID) {
        Document doc = get(docID, Database::kMetaOnly);
        ::slice sequences = doc.body();
        if (sequences.size == 0)
            return false;
        uint64_t seq;
        while (::ReadUVarInt(&sequences, &seq))
            transaction.del((sequence)seq);
        return true;
    }

    bool Index::update(Transaction& transaction,
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


    static Collatable makeRealKey(slice key, slice docID, bool addEllipsis) {
        Collatable realKey;
        realKey.beginArray();
        if (key) {
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

    IndexEnumerator::IndexEnumerator(Index* index, slice startKey, slice startKeyDocID,
                                     slice endKey,   slice endKeyDocID,
                                     const Database::enumerationOptions* options)
    :_index(index)
    {
        bool ascending = startKey.compare(endKey) <= 0;
        Collatable realStartKey = makeRealKey(startKey, startKeyDocID, !ascending);
        Collatable realEndKey   = makeRealKey(endKey,   endKeyDocID,    ascending);
        _dbEnum = _index->Database::enumerate((slice)realStartKey, (slice)realEndKey, options);
        if (options && !options->inclusiveEnd)
            _stopBeforeKey = alloc_slice(realEndKey);
    }

    bool IndexEnumerator::next() {
        if (!_dbEnum.next())
            return false;
        const Document& doc = _dbEnum.doc();

        // Decode the key from collatable form:
        CollatableReader reader(doc.key());
        reader.beginArray();
        _key = reader.read();
        _docID = reader.readString();
        _sequence = reader.readInt();
        
        _value = doc.body();
        return true;
    }

}