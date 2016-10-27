//
//  Index_Test.m
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 5/25/14.
//  Copyright (c) 2014-2016 Couchbase. All rights reserved.
//

#include "Index.hh"
#include "Collatable.hh"

#include "LiteCoreTest.hh"


class IndexTest : public DataFileTestFixture {
public:

    IndexTest(int testOption)
    :DataFileTestFixture(testOption)
    {
        index = new Index(db->getKeyStore("index"));
    }

    ~IndexTest() {
        delete index;
    }

    protected:

        Index* index {nullptr};
        uint64_t _rowCount {0};
        

    void updateDoc(string recordID, vector<string> body, IndexWriter &writer) {
        std::vector<Collatable> keys;
        std::vector<alloc_slice> values;
        for (unsigned i = 1; i < body.size(); i++) {
            CollatableBuilder key;
            key << body[i];
            keys.push_back(key);
            values.push_back(alloc_slice(body[0]));
        }
        bool changed = writer.update(recordID, 1, keys, values, _rowCount);
        REQUIRE(changed);
    }


    uint64_t doQuery() {
        uint64_t nRows = 0;
        for (IndexEnumerator e(*index, Collatable(), litecore::nullslice,
                               Collatable(), litecore::nullslice); e.next(); ) {
            nRows++;
            alloc_slice keyStr = e.key().readString();
            slice valueStr = e.value();
            Debug("key = %.*s, value = %.*s, recordID = %.*s",
                  (int)keyStr.size, (char*)keyStr.buf,
                  (int)valueStr.size, (char*)valueStr.buf,
                  (int)e.recordID().size, (char*)e.recordID().buf);
        }
        REQUIRE(nRows == _rowCount);
        return nRows;
    }
};



N_WAY_TEST_CASE_METHOD (IndexTest, "Index Basics", "[Index]") {
    //LogLevel = kDebug;
    unordered_map<string, vector<string> > records = {
        {"CA", {"California", "San Jose", "San Francisco", "Cambria"}},
        {"WA", {"Washington", "Seattle", "Port Townsend", "Skookumchuk"}},
        {"OR", {"Oregon", "Portland", "Eugene"}},
    };
    {
        Log("--- Populate index");
        Transaction trans(db);
        IndexWriter writer(*index, trans);
        for (auto i : records)
            updateDoc(i.first, i.second, writer);
        trans.commit();
    }

    Log("--- First query");
    REQUIRE(doQuery() == 8);

    {
        Transaction trans(db);
        IndexWriter writer(*index, trans);
        Log("--- Updating OR");
        updateDoc("OR", {"Oregon", "Portland", "Walla Walla", "Salem"}, writer);
        trans.commit();
    }
    REQUIRE(doQuery() == 9);

    {
        Log("--- Removing CA");
        Transaction trans(db);
        IndexWriter writer(*index, trans);
        updateDoc("CA", {}, writer);
        trans.commit();
    }
    REQUIRE(doQuery() == 6);

    Log("--- Reverse enumeration");
    uint64_t nRows = 0;
    IndexEnumerator::Options options;
    options.descending = true;
    for (IndexEnumerator e(*index, Collatable(), litecore::nullslice,
                           Collatable(), litecore::nullslice,
                           options); e.next(); ) {
        nRows++;
        alloc_slice keyStr = e.key().readString();
        Debug("key = %.*s, recordID = %.*s",
              (int)keyStr.size, (char*)keyStr.buf, (int)e.recordID().size, (char*)e.recordID().buf);
    }
    REQUIRE(nRows == 6);
    REQUIRE(_rowCount == nRows);

    // Enumerate a vector of keys:
    Log("--- Enumerating a vector of keys");
    std::vector<KeyRange> keys;
    keys.push_back((Collatable)CollatableBuilder("Cambria"));
    keys.push_back((Collatable)CollatableBuilder("San Jose"));
    keys.push_back((Collatable)CollatableBuilder("Portland"));
    keys.push_back((Collatable)CollatableBuilder("Skookumchuk"));
    nRows = 0;
    for (IndexEnumerator e(*index, keys); e.next(); ) {
        nRows++;
        alloc_slice keyStr = e.key().readString();
        Debug("key = %.*s, recordID = %.*s",
              (int)keyStr.size, (char*)keyStr.buf, (int)e.recordID().size, (char*)e.recordID().buf);
    }
    REQUIRE(nRows == 2);

    // Enumerate a vector of key ranges:
    Log("--- Enumerating a vector of key ranges");
    std::vector<KeyRange> ranges;
    ranges.push_back(KeyRange(CollatableBuilder("Port"), CollatableBuilder(u8"Port\uFFFE")));
    ranges.push_back(KeyRange(CollatableBuilder("Vernon"), CollatableBuilder("Ypsilanti")));
    nRows = 0;
    for (IndexEnumerator e(*index, ranges); e.next(); ) {
        nRows++;
        alloc_slice keyStr = e.key().readString();
        Debug("key = %.*s, recordID = %.*s",
              (int)keyStr.size, (char*)keyStr.buf, (int)e.recordID().size, (char*)e.recordID().buf);
    }
    REQUIRE(nRows == 3);

    // Empty vector:
    ranges.clear();
    nRows = 0;
    for (IndexEnumerator e(*index, ranges); e.next(); ) {
        nRows++;
    }
    REQUIRE(nRows == 0);
}


N_WAY_TEST_CASE_METHOD (IndexTest, "Index DuplicateKeys", "[Index]") {
    Log("--- Populate index");
    {
        Transaction trans(db);
        IndexWriter writer(*index, trans);
        std::vector<Collatable> keys;
        std::vector<alloc_slice> values;
        CollatableBuilder key("Schlage");
        keys.push_back(key);
        values.push_back(alloc_slice("purple"));
        keys.push_back(key);
        values.push_back(alloc_slice("red"));
        bool changed = writer.update("record1"_sl, 1, keys, values, _rowCount);
        REQUIRE(changed);
        REQUIRE(_rowCount == 2);
        trans.commit();
    }
    Log("--- First query");
    REQUIRE(doQuery() == 2);
    {
        Transaction trans(db);
        IndexWriter writer(*index, trans);
        std::vector<Collatable> keys;
        std::vector<alloc_slice> values;
        CollatableBuilder key("Schlage");
        keys.push_back(key);
        values.push_back(alloc_slice("purple"));
        keys.push_back(key);
        values.push_back(alloc_slice("crimson"));
        keys.push_back(CollatableBuilder("Master"));
        values.push_back(alloc_slice("gray"));
        bool changed = writer.update("record1"_sl, 2, keys, values, _rowCount);
        REQUIRE(changed);
        REQUIRE(_rowCount == 3);
        trans.commit();
    }
    Log("--- Second query");
    REQUIRE(doQuery() == 3);
}
