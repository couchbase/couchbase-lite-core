//
//  MapReduce_Test.m
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 5/27/14.
//  Copyright (c) 2014-2016 Couchbase. All rights reserved.
//

#include "MapReduceIndex.hh"
#include "DocEnumerator.hh"
#include "Collatable.hh"
#include "Fleece.hh"

#include "LiteCoreTest.hh"

using namespace fleece;


template <typename T>
static CollatableBuilder ToCollatable(T t) {
    CollatableBuilder c;
    c << t;
    return c;
}


static int numMapCalls;

typedef void (*mapFn)(const Document &doc,
                      std::vector<Collatable> &keys,
                      std::vector<alloc_slice> &values);

static void mapCities(const Document &doc,
                      std::vector<Collatable> &keys,
                      std::vector<alloc_slice> &values)
{
    const Dict *body = Value::fromData(doc.body())->asDict();
    auto name = (string)body->get(slice("name"))->asString();
    const Array *cities = body->get(slice("cities"))->asArray();
    for (Array::iterator i(cities); i; ++i) {
        keys.push_back(ToCollatable(i->asString()));
        values.push_back(ToCollatable(name));
    }
}

static void mapStates(const Document &doc,
                      std::vector<Collatable> &keys,
                      std::vector<alloc_slice> &values)
{
    const Dict *body = Value::fromData(doc.body())->asDict();
    auto name = (string)body->get(slice("name"))->asString();
    const Array *cities = body->get(slice("cities"))->asArray();
    for (Array::iterator i(cities); i; ++i) {
        keys.push_back(ToCollatable(name));
        values.push_back(ToCollatable(i->asString()));
    }
}

static void mapStatesAndCities(const Document &doc,
                      std::vector<Collatable> &keys,
                      std::vector<alloc_slice> &values)
{
    const Dict *body = Value::fromData(doc.body())->asDict();
    auto name = (string)body->get(slice("name"))->asString();
    const Array *cities = body->get(slice("cities"))->asArray();
    for (Array::iterator i(cities); i; ++i) {
        CollatableBuilder key;
        key.beginArray();
        key << name << i->asString();
        key.endArray();
        keys.push_back(key);
        values.push_back(ToCollatable(i->asString()));
    }
}

static void updateIndex(DataFile *indexDB, MapReduceIndex& index, mapFn map) {
    MapReduceIndexer indexer;
    indexer.addIndex(index);
    auto seq = indexer.startingSequence();
    numMapCalls = 0;
    Log("Updating index from sequence=%llu...", seq);

    DocEnumerator::Options options;
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
            (*map)(doc, keys, values);
        }
        indexer.emitDocIntoView(doc.key(), doc.sequence(), 0, keys, values);
    }
    indexer.finished();
    Log("...done updating index (%d map calls)", numMapCalls);
}



class MapReduceTest : public DataFileTestFixture {
    public:

    MapReduceIndex* index;

    MapReduceTest(int testOption)
    :DataFileTestFixture(testOption)
    {
        index = new MapReduceIndex(db->getKeyStore("index"), *db);
        REQUIRE(index);
    }

    ~MapReduceTest() {
        delete index;
    }


    void queryExpectingKeys(vector<string> expectedKeys) {
        updateIndex(db, *index, mapCities);

        size_t nRows = 0;
        for (IndexEnumerator e(*index, Collatable(), litecore::slice::null,
                               Collatable(), litecore::slice::null); e.next(); ) {
            CollatableReader keyReader(e.key());
            alloc_slice keyStr = keyReader.readString();
            Log("key = %s, docID = %.*s",
                keyStr.cString(), (int)e.docID().size, e.docID().buf);
            REQUIRE((string)keyStr == expectedKeys[nRows++]);
        }
        REQUIRE(nRows == expectedKeys.size());
        REQUIRE(index->rowCount() == (uint64_t)nRows);
    }
    
    
    void reducedQueryExpectingKeys(mapFn map,
                                   ReduceFunction &&reduce, unsigned groupLevel,
                                   vector<string> expectedKeyJSON,
                                   vector<string> expectedValueJSON) {
        updateIndex(db, *index, map);

        IndexEnumerator::Options options;
        options.reduce = &reduce;
        options.groupLevel = groupLevel;
        size_t nRows = 0;
        for (IndexEnumerator e(*index,
                               Collatable(), litecore::slice::null,
                               Collatable(), litecore::slice::null,
                               options); e.next(); ) {
            CollatableReader keyReader(e.key());
            std::string keyStr = keyReader.toJSON();
            auto value = Value::fromData(e.value());
            REQUIRE(value);
            auto valueStr = (std::string)value->toJSON();
            Log("key = %s  value = %s", keyStr.c_str(), valueStr.c_str());
            REQUIRE(keyStr == expectedKeyJSON[nRows]);
            REQUIRE(valueStr == expectedValueJSON[nRows]);
            ++nRows;
        }
        REQUIRE(nRows == expectedKeyJSON.size());
    }
    
    
    void addDoc(string docID, string name, vector<string> cities, Transaction &t) {
        Encoder e;
        e.beginDictionary();
            e.writeKey("name");
            e.writeString(name);
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
            t.commit();
        }
        index->setup(0, "1");
    }

};


class CountReduce : public ReduceFunction {
public:
    void operator() (CollatableReader key, slice value) override {
        Log("    CountReduce: key = %s", key.toJSON().c_str());
        ++count;
    }
    alloc_slice reducedValue() override {
        Log("    CountReduce: reduced value = %u", count);
        Encoder e;
        e << count;
        count = 0;
        return e.extractOutput();
    }

private:
    unsigned count {0};
};


N_WAY_TEST_CASE_METHOD (MapReduceTest, "MapReduce", "[MapReduce]") {
    createDocsAndIndex();

    Log("--- First query");
    queryExpectingKeys({"Cambria", "Eugene", "Port Townsend", "Portland",
                                "San Francisco", "San Jose", "Seattle", "Skookumchuk"});
    REQUIRE(numMapCalls == 3);

    Log("--- Updating OR");
    {
        Transaction t(db);
        addDoc("OR", "Oregon", {"Portland", "Walla Walla", "Salem"}, t);
        t.commit();
    }
    queryExpectingKeys({"Cambria", "Port Townsend", "Portland", "Salem",
                                "San Francisco", "San Jose", "Seattle", "Skookumchuk",
                                "Walla Walla"});
    REQUIRE(numMapCalls == 1);

    // After deleting a doc, updating the index can be done incrementally because the deleted doc
    // will appear in the by-sequence iteration, so the indexer can remove its rows.
    Log("--- Deleting CA");
    {
        Transaction t(db);
        store->del(slice("CA"), t);
        t.commit();
    }
    queryExpectingKeys({"Port Townsend", "Portland", "Salem",
                        "Seattle", "Skookumchuk", "Walla Walla"});
    REQUIRE(numMapCalls == 0);

    Log("--- Updating version");
    index->setup(0, "2");
    queryExpectingKeys({"Port Townsend", "Portland", "Salem",
                        "Seattle", "Skookumchuk", "Walla Walla"});
    REQUIRE(numMapCalls == 2);

    // Deletion followed by compaction will purge the deleted docs, so incremental indexing no
    // longer works. The indexer should detect this and rebuild from scratch.
    Log("--- Deleting OR");
    {
        Transaction t(db);
        store->del(slice("OR"), t);
        t.commit();
    }
    Log("--- Compacting db");
    db->compact();

    queryExpectingKeys({"Port Townsend", "Seattle", "Skookumchuk"});
    REQUIRE(numMapCalls == 1);
}


N_WAY_TEST_CASE_METHOD (MapReduceTest, "Reduce", "[MapReduce][Reduce]") {
    createDocsAndIndex();
    reducedQueryExpectingKeys(mapStates, CountReduce(), 0, {"null"}, {"8"});
}

N_WAY_TEST_CASE_METHOD (MapReduceTest, "Group1", "[MapReduce][Reduce]") {
    createDocsAndIndex();
    reducedQueryExpectingKeys(mapStates, CountReduce(), 1,
                              {"\"California\"", "\"Oregon\"", "\"Washington\""},
                              {"3", "2", "3"});
}

N_WAY_TEST_CASE_METHOD (MapReduceTest, "Group1Array", "[MapReduce][Reduce]") {
    createDocsAndIndex();
    reducedQueryExpectingKeys(mapStatesAndCities, CountReduce(), 1,
                              {"[\"California\"]", "[\"Oregon\"]", "[\"Washington\"]"},
                              {"3", "2", "3"});
}


N_WAY_TEST_CASE_METHOD (MapReduceTest, "MapReduce Reopen", "[MapReduce]") {
    createDocsAndIndex();
    updateIndex(db, *index, mapCities);
    sequence lastIndexed = index->lastSequenceIndexed();
    sequence lastChangedAt = index->lastSequenceChangedAt();
    REQUIRE(lastChangedAt > 0);
    REQUIRE(lastIndexed >= lastChangedAt);

    delete index;
    index = NULL;

    index = new MapReduceIndex(db->getKeyStore("index"), *db);
    REQUIRE(index);

    index->setup(0, "1");
    REQUIRE(index->lastSequenceIndexed() == lastIndexed);
    REQUIRE(index->lastSequenceChangedAt() == lastChangedAt);
}
