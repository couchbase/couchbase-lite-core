//
//  Index_Test.m
//  CBForest
//
//  Created by Jens Alfke on 5/25/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#include "Index.hh"
#include "Collatable.hh"

#include "CBForestTest.hh"


class IndexTest : public DatabaseTestFixture {
public:

void setUp() {
    DatabaseTestFixture::setUp();
    index = new Index(db->getKeyStore("index"));
}

void tearDown() {
    delete index;
    DatabaseTestFixture::tearDown();
}

private:

    Index* index {nullptr};
    uint64_t _rowCount {0};
    

void updateDoc(string docID, vector<string> body, IndexWriter &writer) {
    std::vector<Collatable> keys;
    std::vector<alloc_slice> values;
    for (unsigned i = 1; i < body.size(); i++) {
        CollatableBuilder key;
        key << body[i];
        keys.push_back(key);
        values.push_back(alloc_slice(body[0]));
    }
    bool changed = writer.update(docID, 1, keys, values, _rowCount);
    Assert(changed);
}


uint64_t doQuery() {
    uint64_t nRows = 0;
    for (IndexEnumerator e(*index, Collatable(), cbforest::slice::null,
                           Collatable(), cbforest::slice::null,
                           DocEnumerator::Options::kDefault); e.next(); ) {
        nRows++;
        alloc_slice keyStr = e.key().readString();
        slice valueStr = e.value();
        Log("key = %.*s, value = %.*s, docID = %.*s",
              (int)keyStr.size, keyStr.buf,
              (int)valueStr.size, valueStr.buf,
              (int)e.docID().size, e.docID().buf);
    }
    AssertEqual(nRows, _rowCount);
    return nRows;
}


void testBasics() {
    //LogLevel = kDebug;
    unordered_map<string, vector<string> > docs = {
        {"CA", {"California", "San Jose", "San Francisco", "Cambria"}},
        {"WA", {"Washington", "Seattle", "Port Townsend", "Skookumchuk"}},
        {"OR", {"Oregon", "Portland", "Eugene"}},
    };
    {
        Log("--- Populate index");
        Transaction trans(db);
        IndexWriter writer(*index, trans);
        for (auto i : docs)
            updateDoc(i.first, i.second, writer);
    }

    Log("--- First query");
    AssertEqual(doQuery(), 8ull);

    {
        Transaction trans(db);
        IndexWriter writer(*index, trans);
        Log("--- Updating OR");
        updateDoc("OR", {"Oregon", "Portland", "Walla Walla", "Salem"}, writer);
    }
    AssertEqual(doQuery(), 9ull);

    {
        Log("--- Removing CA");
        Transaction trans(db);
        IndexWriter writer(*index, trans);
        updateDoc("CA", {}, writer);
    }
    AssertEqual(doQuery(), 6ull);

    Log("--- Reverse enumeration");
    uint64_t nRows = 0;
    auto options = DocEnumerator::Options::kDefault;
    options.descending = true;
    for (IndexEnumerator e(*index, Collatable(), cbforest::slice::null,
                           Collatable(), cbforest::slice::null,
                           options); e.next(); ) {
        nRows++;
        alloc_slice keyStr = e.key().readString();
        Log("key = %.*s, docID = %.*s",
              (int)keyStr.size, keyStr.buf, (int)e.docID().size, e.docID().buf);
    }
    AssertEqual(nRows, 6ull);
    AssertEqual(_rowCount, nRows);

    // Enumerate a vector of keys:
    Log("--- Enumerating a vector of keys");
    std::vector<KeyRange> keys;
    keys.push_back((Collatable)CollatableBuilder("Cambria"));
    keys.push_back((Collatable)CollatableBuilder("San Jose"));
    keys.push_back((Collatable)CollatableBuilder("Portland"));
    keys.push_back((Collatable)CollatableBuilder("Skookumchuk"));
    nRows = 0;
    for (IndexEnumerator e(*index, keys, DocEnumerator::Options::kDefault); e.next(); ) {
        nRows++;
        alloc_slice keyStr = e.key().readString();
        Log("key = %.*s, docID = %.*s",
              (int)keyStr.size, keyStr.buf, (int)e.docID().size, e.docID().buf);
    }
    AssertEqual(nRows, 2ull);

    // Enumerate a vector of key ranges:
    Log("--- Enumerating a vector of key ranges");
    std::vector<KeyRange> ranges;
    ranges.push_back(KeyRange(CollatableBuilder("Port"), CollatableBuilder("Port\uFFFE")));
    ranges.push_back(KeyRange(CollatableBuilder("Vernon"), CollatableBuilder("Ypsilanti")));
    nRows = 0;
    for (IndexEnumerator e(*index, ranges, DocEnumerator::Options::kDefault); e.next(); ) {
        nRows++;
        alloc_slice keyStr = e.key().readString();
        Log("key = %.*s, docID = %.*s",
              (int)keyStr.size, keyStr.buf, (int)e.docID().size, e.docID().buf);
    }
    AssertEqual(nRows, 3ull);

    // Empty vector:
    ranges.clear();
    nRows = 0;
    for (IndexEnumerator e(*index, ranges, DocEnumerator::Options::kDefault); e.next(); ) {
        nRows++;
    }
    AssertEqual(nRows, 0ull);
}

void testDuplicateKeys() {
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
        bool changed = writer.update(slice("doc1"), 1, keys, values, _rowCount);
        Assert(changed);
        AssertEqual(_rowCount, 2ull);
    }
    Log("--- First query");
    AssertEqual(doQuery(), 2ull);
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
        bool changed = writer.update(slice("doc1"), 2, keys, values, _rowCount);
        Assert(changed);
        AssertEqual(_rowCount, 3ull);
    }
    Log("--- Second query");
    AssertEqual(doQuery(), 3ull);
}


    CPPUNIT_TEST_SUITE( IndexTest );
    CPPUNIT_TEST( testBasics );
    CPPUNIT_TEST( testDuplicateKeys );
    CPPUNIT_TEST_SUITE_END();
};

CPPUNIT_TEST_SUITE_REGISTRATION(IndexTest);
