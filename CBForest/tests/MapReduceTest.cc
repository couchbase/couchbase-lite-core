//
//  MapReduce_Test.m
//  CBForest
//
//  Created by Jens Alfke on 5/27/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#include "MapReduceIndex.hh"
#include "DocEnumerator.hh"
#include "Collatable.hh"
#include "Fleece.hh"

#include "CBForestTest.hh"

using namespace fleece;


template <typename T>
static CollatableBuilder ToCollatable(T t) {
    CollatableBuilder c;
    c << t;
    return c;
}


static int numMapCalls;

static void updateIndex(DataFile *indexDB, MapReduceIndex& index) {
    MapReduceIndexer indexer;
    indexer.addIndex(index);
    auto seq = indexer.startingSequence();
    numMapCalls = 0;
    Log("Updating index from sequence=%llu...", seq);

    auto options = DocEnumerator::Options::kDefault;
    options.includeDeleted = true;
    DocEnumerator e(index.sourceStore(), seq, UINT64_MAX, options);
    while (e.next()) {
        auto &doc = e.doc();
        Log("    enumerating seq %llu: '%.*s' (del=%d)",
              doc.sequence(), (int)doc.key().size, doc.key().buf, doc.deleted());
        std::vector<Collatable> keys;
        std::vector<alloc_slice> values;
        if (!doc.deleted()) {
            // Here's the pseudo map function:
            ++numMapCalls;
            const Dict *body = Value::fromData(doc.body())->asDict();
            auto name = (string)body->get(slice("name"))->asString();
            const Array *cities = body->get(slice("cities"))->asArray();
            for (Array::iterator i(cities); i; ++i) {
                keys.push_back(ToCollatable(i->asString()));
                values.push_back(ToCollatable(name));
            }
        }
        indexer.emitDocIntoView(doc.key(), doc.sequence(), 0, keys, values);
    }
    indexer.finished();
    Log("...done updating index (%d map calls)", numMapCalls);
}



class MapReduceTest : public DataFileTestFixture {
public:

MapReduceIndex* index;

void setUp() {
    DataFileTestFixture::setUp();
    index = new MapReduceIndex(db->getKeyStore("index"), *db);
    Assert(index);
}

void tearDown() {
    delete index;
    DataFileTestFixture::tearDown();
}


void queryExpectingKeys(vector<string> expectedKeys) {
    updateIndex(db, *index);

    size_t nRows = 0;
    for (IndexEnumerator e(*index, Collatable(), cbforest::slice::null,
                           Collatable(), cbforest::slice::null,
                           DocEnumerator::Options::kDefault); e.next(); ) {
        CollatableReader keyReader(e.key());
        alloc_slice keyStr = keyReader.readString();
        Log("key = %s, docID = %.*s",
              keyStr.cString(), (int)e.docID().size, e.docID().buf);
        AssertEqual((string)keyStr, expectedKeys[nRows++]);
    }
    AssertEqual(nRows, expectedKeys.size());
    AssertEqual(index->rowCount(), (uint64_t)nRows);
}


void addDoc(string docID, string name, vector<string> cities, Transaction &t) {
    Encoder e;
    e.beginDictionary();
        e.writeKey("name");
        e.writeString("California");
        e.writeKey("cities");
        e.beginArray();
            for (auto city : cities)
                e.writeString(city);
        e.endArray();
    e.endDictionary();
    auto data = e.extractOutput();

    store->set(docID, slice::null, data, t);
}


void createDocsAndIndex() {
    {
        Transaction t(db);
        addDoc("CA", "California", {"San Jose", "San Francisco", "Cambria"}, t);
        addDoc("WA", "Washington", {"Seattle", "Port Townsend", "Skookumchuk"}, t);
        addDoc("OR", "Oregon",     {"Portland", "Eugene"}, t);
    }
    index->setup(0, "1");
}


void testMapReduce() {
    createDocsAndIndex();

    Log("--- First query");
    queryExpectingKeys({"Cambria", "Eugene", "Port Townsend", "Portland",
                                "San Francisco", "San Jose", "Seattle", "Skookumchuk"});
    AssertEqual(numMapCalls, 3);

    Log("--- Updating OR");
    {
        Transaction t(db);
        addDoc("OR", "Oregon", {"Portland", "Walla Walla", "Salem"}, t);
    }
    queryExpectingKeys({"Cambria", "Port Townsend", "Portland", "Salem",
                                "San Francisco", "San Jose", "Seattle", "Skookumchuk",
                                "Walla Walla"});
    AssertEqual(numMapCalls, 1);

    // After deleting a doc, updating the index can be done incrementally because the deleted doc
    // will appear in the by-sequence iteration, so the indexer can remove its rows.
    Log("--- Deleting CA");
    {
        Transaction t(db);
        store->del(slice("CA"), t);
    }
    queryExpectingKeys({"Port Townsend", "Portland", "Salem",
                        "Seattle", "Skookumchuk", "Walla Walla"});
    AssertEqual(numMapCalls, 0);

    Log("--- Updating version");
    index->setup(0, "2");
    queryExpectingKeys({"Port Townsend", "Portland", "Salem",
                        "Seattle", "Skookumchuk", "Walla Walla"});
    AssertEqual(numMapCalls, 2);

    // Deletion followed by compaction will purge the deleted docs, so incremental indexing no
    // longer works. The indexer should detect this and rebuild from scratch.
    Log("--- Deleting OR");
    {
        Transaction t(db);
        store->del(slice("OR"), t);
    }
    Log("--- Compacting db");
    db->compact();

    queryExpectingKeys({"Port Townsend", "Seattle", "Skookumchuk"});
    AssertEqual(numMapCalls, 1);
}

void testReopen() {
    createDocsAndIndex();
    updateIndex(db, *index);
    sequence lastIndexed = index->lastSequenceIndexed();
    sequence lastChangedAt = index->lastSequenceChangedAt();
    Assert(lastChangedAt > 0);
    Assert(lastIndexed >= lastChangedAt);

    delete index;
    index = NULL;

    index = new MapReduceIndex(db->getKeyStore("index"), *db);
    Assert(index);

    index->setup(0, "1");
    AssertEqual(index->lastSequenceIndexed(), lastIndexed);
    AssertEqual(index->lastSequenceChangedAt(), lastChangedAt);
}


    CPPUNIT_TEST_SUITE( MapReduceTest );
    CPPUNIT_TEST( testMapReduce );
    CPPUNIT_TEST( testReopen );
    CPPUNIT_TEST_SUITE_END();
};

CPPUNIT_TEST_SUITE_REGISTRATION(MapReduceTest);
