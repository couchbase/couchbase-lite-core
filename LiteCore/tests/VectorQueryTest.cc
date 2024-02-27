//
// VectorQueryTest.cc
//
// Copyright © 2023 Couchbase. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "VectorQueryTest.hh"

#ifdef COUCHBASE_ENTERPRISE

class SIFTVectorQueryTest : public VectorQueryTest {
  public:
    SIFTVectorQueryTest(int which) : VectorQueryTest(which) {}

    void createVectorIndex() {
        IndexSpec::VectorOptions options(128);
        options.clustering.type           = IndexSpec::VectorOptions::Flat;
        options.clustering.flat_centroids = 256;
        VectorQueryTest::createVectorIndex("vecIndex", "[ ['.vector'] ]", options);
    }

    void readVectorDocs(size_t maxLines = 1000000) {
        ExclusiveTransaction t(db);
        size_t               docNo = 0;
        ReadFileByLines(
                TestFixture::sFixturesDir + "vectors_128x10000.json",
                [&](FLSlice line) {
                    writeDoc(
                            stringWithFormat("rec-%04zu", ++docNo), {}, t,
                            [&](Encoder& enc) {
                                JSONConverter conv(enc);
                                REQUIRE(conv.encodeJSON(line));
                            },
                            false);
                    return true;
                },
                maxLines);
        t.commit();
    }

    // Create the $target query param. (This happens to be equal to the vector in rec-0010.)
    static constexpr float const kTargetVector[128] = {
            21, 13, 18, 11, 14, 6,   4,   14, 39,  54, 52, 10, 8,  14,  5,  2,  23, 76,  65,  10,  11,  23,
            3,  0,  6,  10, 17, 5,   7,   21, 20,  13, 63, 7,  25, 13,  4,  12, 13, 112, 109, 112, 63,  21,
            2,  1,  1,  40, 25, 43,  41,  98, 112, 49, 7,  5,  18, 57,  24, 14, 62, 49,  34,  29,  100, 14,
            3,  1,  5,  14, 7,  92,  112, 14, 28,  5,  9,  34, 79, 112, 18, 15, 20, 29,  75,  112, 112, 50,
            6,  61, 45, 13, 33, 112, 77,  4,  18,  17, 5,  3,  4,  5,   4,  15, 28, 4,   6,   1,   7,   33,
            86, 71, 3,  8,  5,  4,   16,  72, 83,  10, 5,  40, 3,  0,   1,  51, 36, 3};

    static constexpr const char* const kFTSSentences[5] = {
            "FTS5 is an SQLite virtual table module that provides full-text search functionality to database "
            "applications.",
            "In their most elementary form, full-text search engines allow the user to efficiently search a large "
            "collection of documents for the subset that contain one or more instances of a search term.",
            "The search functionality provided to world wide web users by Google is, among other things, a "
            "full-text search engine, as it allows users to search for all documents on the web that contain, for "
            "example, the term \"fts5\".",
            "To use FTS5, the user creates an FTS5 virtual table with one or more columns.",
            "Looking for things, searching for things, going on adventures..."};
};

N_WAY_TEST_CASE_METHOD(SIFTVectorQueryTest, "Create/Delete Vector Index", "[Query][.VectorSearch]") {
    auto allKeyStores = db->allKeyStoreNames();
    readVectorDocs(1);
    createVectorIndex();
    CHECK(db->allKeyStoreNames() == allKeyStores);  // CBL-3824, CBL-5369
    // Delete a doc too:
    {
        ExclusiveTransaction t(db);
        store->del("rec-0001", t);
        t.commit();
    }
    store->deleteIndex("vecIndex"_sl);
    CHECK(db->allKeyStoreNames() == allKeyStores);
}

N_WAY_TEST_CASE_METHOD(SIFTVectorQueryTest, "Query Vector Index", "[Query][.VectorSearch]") {
    readVectorDocs();
    {
        // Add some docs without vector data, to ensure that doesn't break indexing:
        ExclusiveTransaction t(db);
        writeMultipleTypeDocs(t);
        t.commit();
    }

    createVectorIndex();

    string queryStr = R"(
        ['SELECT', {
            WHERE:    ['VECTOR_MATCH()', 'vecIndex', ['$target'], 5],
            WHAT:     [ ['._id'], ['AS', ['VECTOR_DISTANCE()', 'vecIndex'], 'distance'] ],
            ORDER_BY: [ ['.distance'] ],
         }] )";

    Retained<Query> query{store->compileQuery(json5(queryStr), QueryLanguage::kJSON)};
    REQUIRE(query != nullptr);

    // Create the $target query param. (This happens to be equal to the vector in rec-0010.)
    Encoder enc;
    enc.beginDictionary();
    enc.writeKey("target");
    enc.writeData(slice(kTargetVector, sizeof(kTargetVector)));
    enc.endDictionary();
    Query::Options options(enc.finish());

    // Run the query:
    Retained<QueryEnumerator> e(query->createEnumerator(&options));
    REQUIRE(e->getRowCount() == 5);  // the call to VECTOR_MATCH requested only 5 results

    // The `expectedDistances` array contains the exact distances.
    // Vector encoders are lossy, so using one in the index will result in approximate distances,
    // which is why the distance check below is so loose.
    static constexpr slice expectedIDs[5]       = {"rec-0010", "rec-0031", "rec-0022", "rec-0012", "rec-0020"};
    static constexpr float expectedDistances[5] = {0, 4172, 10549, 29275, 32025};

    for ( size_t i = 0; i < 5; ++i ) {
        REQUIRE(e->next());
        slice id       = e->columns()[0]->asString();
        float distance = e->columns()[1]->asFloat();
        INFO("i=" << i);
        CHECK(id == expectedIDs[i]);
        CHECK_THAT(distance, Catch::Matchers::WithinRel(expectedDistances[i], 0.20f)
                                     || Catch::Matchers::WithinAbs(expectedDistances[i], 400.0f));
    }
    CHECK(!e->next());
    Log("done");

    reopenDatabase();
}

// Test joining the result of VECTOR_MATCH with a property of another collection. In particular, it joins
// the result of the previous test, "Query Vector Index", with "other" collection that refers to the doc IDs
// from VECTOR_MATCH.
N_WAY_TEST_CASE_METHOD(SIFTVectorQueryTest, "Query Vector Index with Join", "[Query][.VectorSearch]") {
    readVectorDocs();
    {
        // Add some docs without vector data, to ensure that doesn't break indexing:
        ExclusiveTransaction t(db);
        writeMultipleTypeDocs(t);
        t.commit();
    }
    createVectorIndex();

    // Collection "other"
    KeyStore* otherStore = &db->getKeyStore(".other");
    {
        ExclusiveTransaction t(db);
        writeDoc(*otherStore, "doc01", DocumentFlags::kNone, t, [=](Encoder& enc) {
            enc.writeKey("refID");
            enc.writeString("rec-0031");
            enc.writeKey("publisher");
            enc.writeString("Couchbase");
        });
        writeDoc(*otherStore, "doc02", DocumentFlags::kNone, t, [=](Encoder& enc) {
            enc.writeKey("refID");
            enc.writeString("rec-0011");  // this is not fetched by vector_match, c.f. "Query Vector Index"
            enc.writeKey("publisher");
            enc.writeString("Microsoft");
        });
        writeDoc(*otherStore, "doc03", DocumentFlags::kNone, t, [=](Encoder& enc) {
            enc.writeKey("refID");
            enc.writeString("rec-0012");
            enc.writeKey("publisher");
            enc.writeString("Apple");
        });
        t.commit();
    }

    string queryStr = R"(SELECT META(a).id, other.publisher FROM )"s + collectionName;
    queryStr += R"( AS a JOIN other ON META(a).id = other.refID )"
                R"(WHERE VECTOR_MATCH(a.vecIndex, $target, 5) )";

    Retained<Query> query{store->compileQuery(queryStr, QueryLanguage::kN1QL)};
    REQUIRE(query != nullptr);

    // Create the $target query param. (This happens to be equal to the vector in rec-0010.)
    // Same target as used by test "Query Vector Index"
    Encoder enc;
    enc.beginDictionary();
    enc.writeKey("target");
    enc.writeData(slice(kTargetVector, sizeof(kTargetVector)));
    enc.endDictionary();
    Query::Options options(enc.finish());

    // Run the query:
    Retained<QueryEnumerator> e(query->createEnumerator(&options));
    REQUIRE(e->getRowCount() == 2);  // the call to VECTOR_MATCH requested 5 results. Two of them passed JOIN clause.

    // c.f. test "Query Vector Index". "rec-0031" and "rec-0012" are fetched by vector_match.
    static constexpr slice expectedIDs[2]  = {"rec-0031", "rec-0012"};
    static constexpr slice expectedPubs[2] = {"Couchbase", "Apple"};

    size_t i = 0;
    while ( e->next() ) {
        slice id        = e->columns()[0]->asString();
        slice publisher = e->columns()[1]->asString();
        CHECK(id == expectedIDs[i]);
        CHECK(publisher == expectedPubs[i++]);
    }
    CHECK(i == 2);
}

// Join the result of VECTOR_MATCH and FTS MATCH.
// VECTOR_MATCH fetches {"rec-0010", "rec-0031", "rec-0022", "rec-0012", "rec-0020"}, c.f. "Query Vector Index".
// FTS MATCH fetches {"doc02", "doc03", "doc01", "doc05"}, c.f."Query Full-Text English_US",
// and only 3 of them refer to doc IDs in the result of VECTOR_MATCH.
// Hence the joined result includes 3 rows.
N_WAY_TEST_CASE_METHOD(SIFTVectorQueryTest, "Query Vector Index and Join with FTS", "[Query][.VectorSearch]") {
    readVectorDocs();
    {
        // Add some docs without vector data, to ensure that doesn't break indexing:
        ExclusiveTransaction t(db);
        writeMultipleTypeDocs(t);
        t.commit();
    }
    createVectorIndex();

    // Collection "other"
    KeyStore* otherStore = &db->getKeyStore(".other");
    {
        // C.f. test "Query Full-Text English_US"
        ExclusiveTransaction t(db);
        writeDoc(*otherStore, "doc01", DocumentFlags::kNone, t, [=](Encoder& enc) {
            enc.writeKey("refID");
            enc.writeString("rec-0031");
            enc.writeKey("sentence");
            enc.writeString(kFTSSentences[0]);
        });
        writeDoc(*otherStore, "doc02", DocumentFlags::kNone, t, [=](Encoder& enc) {
            enc.writeKey("refID");
            // "rec-0011" is not in the result of VECTOR_MATCH
            enc.writeString("rec-0011");
            enc.writeKey("sentence");
            enc.writeString(kFTSSentences[1]);
        });
        // "doc03" is not in the result of FTS MATCH
        writeDoc(*otherStore, "doc03", DocumentFlags::kNone, t, [=](Encoder& enc) {
            enc.writeKey("refID");
            enc.writeString("rec-0012");
            enc.writeKey("sentence");
            enc.writeString(kFTSSentences[2]);
        });
        writeDoc(*otherStore, "doc04", DocumentFlags::kNone, t, [=](Encoder& enc) {
            enc.writeKey("refID");
            enc.writeString("rec-0020");
            enc.writeKey("sentence");
            enc.writeString(kFTSSentences[3]);
        });
        writeDoc(*otherStore, "doc05", DocumentFlags::kNone, t, [=](Encoder& enc) {
            enc.writeKey("refID");
            enc.writeString("rec-0022");
            enc.writeKey("sentence");
            enc.writeString(kFTSSentences[4]);
        });
        t.commit();
    }
    otherStore->createIndex("sentence", "[[\".sentence\"]]", IndexSpec::kFullText,
                            IndexSpec::FTSOptions{"english", true});

    string queryStr = R"(SELECT META(a).id, META(other).id FROM )"s + collectionName;
    queryStr += R"( AS a JOIN other ON META(a).id = other.refID )"
                R"(WHERE VECTOR_MATCH(a.vecIndex, $target, 5) AND MATCH(other.sentence, "search") )";

    Retained<Query> query{store->compileQuery(queryStr, QueryLanguage::kN1QL)};
    REQUIRE(query != nullptr);

    // Create the $target query param. (This happens to be equal to the vector in rec-0010.)
    // Same target as used by test "Query Vector Index"
    Encoder enc;
    enc.beginDictionary();
    enc.writeKey("target");
    enc.writeData(slice(kTargetVector, sizeof(kTargetVector)));
    enc.endDictionary();
    Query::Options options(enc.finish());

    // Run the query:
    Retained<QueryEnumerator> e(query->createEnumerator(&options));
    REQUIRE(e->getRowCount() == 3);

    // VECTOR_MATCH will fecth these docs: {"rec-0010", "rec-0031", "rec-0022", "rec-0012", "rec-0020"}
    // FTS MATCH will fetch {"doc02", "doc03", "doc01", "doc05"}
    // "doc03" does not refer to any in result of VECTOR_MATCH.
    static constexpr slice expectedID1s[] = {"rec-0031", "rec-0022", "rec-0012"};
    static constexpr slice expectedID2s[] = {"doc01", "doc05", "doc03"};

    size_t i = 0;
    while ( e->next() ) {
        slice id1 = e->columns()[0]->asString();
        slice id2 = e->columns()[1]->asString();
        CHECK(id1 == expectedID1s[i]);
        CHECK(id2 == expectedID2s[i++]);
    }
    CHECK(i == 3);
}

// Test intersection of vector-search and FTS
// The db table has two columns, vector and sentence. Vector is indexed by the VectorIndex,
// and sentence is indexed by FTS.
// VectorIndex picks 5 docs, {"rec-0010", "rec-0031", "rec-0022", "rec-0012", "rec-0020"}
// FTS picks 4 sentences, We pair vectors (1000 rows) and 5 sentences by cycling the sentences
// except for the docs that are picked by VS to ensure they have different sentences.
// The intersection should have 4 docs.
N_WAY_TEST_CASE_METHOD(SIFTVectorQueryTest, "Query Vector Index and AND with FTS", "[Query][.VectorSearch]") {
    {
        ExclusiveTransaction t(db);
        size_t               docNo = 0;
        ReadFileByLines(
                TestFixture::sFixturesDir + "vectors_128x10000.json",
                [&](FLSlice line) {
                    Encoder       enc;
                    JSONConverter conv(enc);
                    conv.encodeJSON(line);
                    alloc_slice   body  = enc.finish();
                    Retained<Doc> doc   = new Doc(body, Doc::kTrusted, nullptr);
                    auto          root  = doc->asDict();
                    auto          v     = root->get("vector");
                    string        docID = stringWithFormat("rec-%04zu", ++docNo);
                    // Vector-search will pick the following IDs,
                    // {"rec-0010", "rec-0031", "rec-0022", "rec-0012", "rec-0020"}
                    // or in docNo,
                    // [10, 31, 22, 12, 20] (docId-1)%5 =>
                    // [2. 0, 3. 1. 4]
                    writeDoc(
                            docID, {}, t,
                            [&](Encoder& enc) {
                                enc.beginDictionary();
                                enc.writeKey("vector");
                                enc.writeValue(v);
                                enc.writeKey("sentence");
                                switch ( docNo ) {
                                    case 10:
                                        enc.writeString(kFTSSentences[2]);
                                        break;
                                    case 31:
                                        enc.writeString(kFTSSentences[0]);
                                        break;
                                    case 22:
                                        // this sentence is not selected by FTS
                                        enc.writeString(kFTSSentences[3]);
                                        break;
                                    case 12:
                                        enc.writeString(kFTSSentences[1]);
                                        break;
                                    case 20:
                                        enc.writeString(kFTSSentences[4]);
                                        break;
                                    default:
                                        enc.writeString(kFTSSentences[(docNo - 1) % 5]);
                                        break;
                                }
                                enc.endDictionary();
                            },
                            false);
                    return true;
                },
                1000000);
        t.commit();
    }
    createVectorIndex();
    store->createIndex("sentence", "[[\".sentence\"]]", IndexSpec::kFullText, IndexSpec::FTSOptions{"english", true});

    string queryStr =
            R"(SELECT META(a).id, VECTOR_DISTANCE(a.vecIndex) AS distance, a.sentence FROM )"s + collectionName;
    queryStr += R"( AS a WHERE VECTOR_MATCH(a.vecIndex, $target, 5))";
    queryStr += R"( AND MATCH(a.sentence, "search"))";

    Retained<Query> query{store->compileQuery(queryStr, QueryLanguage::kN1QL)};
    REQUIRE(query != nullptr);

    Encoder enc;
    enc.beginDictionary();
    enc.writeKey("target");
    enc.writeData(slice(kTargetVector, sizeof(kTargetVector)));
    enc.endDictionary();
    Query::Options options(enc.finish());

    // Run the query:
    Retained<QueryEnumerator> e(query->createEnumerator(&options));
    CHECK(e->getRowCount() == 4);
    CHECK(query->columnCount() == 3);

    std::map<std::string, std::string> checks{{"rec-0010", kFTSSentences[2]},
                                              {"rec-0031", kFTSSentences[0]},
                                              {"rec-0012", kFTSSentences[1]},
                                              {"rec-0020", kFTSSentences[4]}};
    while ( e->next() ) {
        string docID = e->columns()[0]->asString().asString();
        auto   iter  = checks.find(docID);
        CHECK(iter != checks.end());
        string sentence = e->columns()[2]->asString().asString();
        CHECK(iter->second == sentence);
        checks.erase(iter);
    }
    CHECK(checks.size() == 0);
}

#endif
