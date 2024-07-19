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
#include "Base64.hh"
#include "c4Database.hh"
#include "c4Collection.hh"
#include "c4Index.h"
#include "c4Database.h"

#ifdef COUCHBASE_ENTERPRISE

class SIFTVectorQueryTest : public VectorQueryTest {
  public:
    SIFTVectorQueryTest(int which) : VectorQueryTest(which) {}

    SIFTVectorQueryTest() : VectorQueryTest(0) {}

    IndexSpec::VectorOptions vectorIndexOptions() const {
        return IndexSpec::VectorOptions(128, vectorsearch::FlatClustering{256}, IndexSpec::DefaultEncoding);
    }

    void createVectorIndex() {
        VectorQueryTest::createVectorIndex("vecIndex", "[ ['.vector'] ]", vectorIndexOptions());
    }

    enum struct VectorType : uint8_t {
        Array,   // array of floats
        String,  // Base64 encoded of the float array. Must be little-endian
        Mixed    // Mixed of above
    };

    void readVectorDocs(size_t maxLines = 1000000, VectorType type = VectorType::Array) {
        ExclusiveTransaction  t(db);
        size_t                docNo = 0;
        constexpr const char* kVectorJSON[]{"vectors_128x10000.json", "vectors_base64_128x10000.json"};
        if ( type < VectorType::Mixed ) {
            ReadFileByLines(
                    TestFixture::sFixturesDir + kVectorJSON[(int)type],
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
        } else if ( type == VectorType::Mixed ) {
            std::vector<alloc_slice> arrayVec;
            std::vector<alloc_slice> stringVec;
            ReadFileByLines(
                    TestFixture::sFixturesDir + kVectorJSON[(int)VectorType::Array],
                    [&](FLSlice line) {
                        arrayVec.emplace_back(line);
                        return true;
                    },
                    maxLines);
            ReadFileByLines(
                    TestFixture::sFixturesDir + kVectorJSON[(int)VectorType::String],
                    [&](FLSlice line) {
                        stringVec.emplace_back(line);
                        return true;
                    },
                    maxLines);
            REQUIRE(arrayVec.size() == stringVec.size());

            for ( docNo = 0; docNo < arrayVec.size(); ++docNo ) {
                if ( docNo % 2 == 0 ) {
                    writeDoc(
                            stringWithFormat("rec-%04zu", docNo + 1), {}, t,
                            [&](Encoder& enc) {
                                JSONConverter conv(enc);
                                REQUIRE(conv.encodeJSON(arrayVec[docNo]));
                            },
                            false);
                } else {
                    writeDoc(
                            stringWithFormat("rec-%04zu", docNo + 1), {}, t,
                            [&](Encoder& enc) {
                                JSONConverter conv(enc);
                                REQUIRE(conv.encodeJSON(stringVec[docNo]));
                            },
                            false);
                }
            }

            t.commit();
        }
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
    const VectorType type = GENERATE(VectorType::Array, VectorType::String);
    logSection(type == VectorType::Array ? "Vector Type: array" : "Vector Type: string", 1);

    auto allKeyStores = db->allKeyStoreNames();
    readVectorDocs(1, type);
    createVectorIndex();

    // Recover the IndexSpec:
    std::optional<IndexSpec> spec = store->getIndex("vecIndex");
    REQUIRE(spec);
    CHECK(spec->name == "vecIndex");
    CHECK(spec->type == IndexSpec::kVector);
    auto vecOptions = spec->vectorOptions();
    REQUIRE(vecOptions);
    auto trueOptions = vectorIndexOptions();
    CHECK(vecOptions->dimensions == trueOptions.dimensions);
    CHECK(vecOptions->clusteringType() == trueOptions.clusteringType());
    CHECK(vecOptions->encodingType() == trueOptions.encodingType());

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
    const VectorType type = GENERATE(VectorType::Array, VectorType::String, VectorType::Mixed);
    logSection(type == VectorType::Array    ? "Vector Type: array"
               : type == VectorType::String ? "Vector Type: string"
                                            : "Vector Type: mixed",
               1);

    readVectorDocs(1000000, type);
    {
        // Add some docs without vector data, to ensure that doesn't break indexing:
        ExclusiveTransaction t(db);
        writeMultipleTypeDocs(t);
        t.commit();
    }

    createVectorIndex();

    {
        // Number of results = 10
        string          queryStr = R"(
         ['SELECT', {
            WHAT:     [ ['._id'], ['AS', ['APPROX_VECTOR_DISTANCE()', ['.vector'], ['$target']], 'distance'] ],
            ORDER_BY: [ ['.distance'] ],
            LIMIT:    10
         }] )";
        Retained<Query> query{store->compileQuery(json5(queryStr), QueryLanguage::kJSON)};

        Log("---- Querying with $target = data");
        Query::Options options = optionsWithTargetVector(kTargetVector, kData);
        checkExpectedResults(query->createEnumerator(&options),
                             {"rec-0010", "rec-0031", "rec-0022", "rec-0012", "rec-0020", "rec-0076", "rec-0087",
                              "rec-3327", "rec-1915", "rec-8265"},
                             {0, 4172, 10549, 29275, 32025, 65417, 67313, 68009, 70231, 70673});
    }

    // Number of Results = 5
    string          queryStr = R"(
     ['SELECT', {
       WHAT:     [ ['._id'], ['AS', ['APPROX_VECTOR_DISTANCE()', ['.vector'], ['$target']], 'distance'] ],
       ORDER_BY: [ ['.distance'] ],
       LIMIT:    5
     }] )";
    Retained<Query> query{store->compileQuery(json5(queryStr), QueryLanguage::kJSON)};

    static constexpr valueType kParamTypes[] = {kData, kString, kArray};
    for ( valueType asType : kParamTypes ) {
        Log("---- Querying with $target of Fleece type %d", int(asType));
        Query::Options options = optionsWithTargetVector(kTargetVector, asType);
        checkExpectedResults(query->createEnumerator(&options),
                             {"rec-0010", "rec-0031", "rec-0022", "rec-0012", "rec-0020"},
                             {0, 4172, 10549, 29275, 32025});
    }

    {
        // Update a document with an invalid vector property:
        {
            Log("---- Updating rec-0031 to remove its vector");
            ExclusiveTransaction t(db);
            writeDoc("rec-0031", DocumentFlags::kNone, t, [=](Encoder& enc) {
                enc.writeKey("vector");
                enc.writeString("nope");
            });
            t.commit();
            ++expectedWarningsLogged;
        }
        // Verify the updated document is missing from the results:
        Query::Options options = optionsWithTargetVector(kTargetVector, kData);
        checkExpectedResults(query->createEnumerator(&options),
                             {"rec-0010", "rec-0022", "rec-0012", "rec-0020", "rec-0076"},
                             {0, 10549, 29275, 32025, 65417});
    }

    reopenDatabase();
}

N_WAY_TEST_CASE_METHOD(SIFTVectorQueryTest, "Hybrid Vector Query", "[Query][.VectorSearch]") {
    readVectorDocs();
    {
        // Add some docs without vector data, to ensure that doesn't break indexing:
        ExclusiveTransaction t(db);
        writeMultipleTypeDocs(t);
        t.commit();
    }
    createVectorIndex();

    string          queryStr = R"(
     ['SELECT', {
        WHERE:    ['=', 0, ['%', ['._sequence'], 100]],
        WHAT:     [ ['._id'], ['AS', ['APPROX_VECTOR_DISTANCE()', ['.vector'], ['$target']], 'distance'] ],
        ORDER_BY: [ ['.distance'] ],
        LIMIT:    10
     }] )";
    Retained<Query> query{store->compileQuery(json5(queryStr), QueryLanguage::kJSON)};

    Log("---- Querying with $target = data");
    Query::Options options = optionsWithTargetVector(kTargetVector, kData);
    checkExpectedResults(query->createEnumerator(&options),
                         {"rec-5300", "rec-4900", "rec-7100", "rec-3600", "rec-8700", "rec-8500", "rec-2400",
                          "rec-4700", "rec-4300", "rec-2600"},
                         {85776, 90431, 92142, 92629, 94598, 94989, 104787, 106750, 113260, 116129});
}

// Test joining the result of VECTOR_MATCH with a property of another collection. In particular, it joins
// the result of the previous test, "Query Vector Index", with "other" collection that refers to the doc IDs
// from VECTOR_MATCH.
N_WAY_TEST_CASE_METHOD(SIFTVectorQueryTest, "Query Vector Index with Join", "[Query][.VectorSearch]") {
    readVectorDocs(1000000, VectorType::String);
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
                R"(ORDER BY APPROX_VECTOR_DISTANCE(a.vector, $target) LIMIT 5 )";

    Retained<Query> query{store->compileQuery(queryStr, QueryLanguage::kN1QL)};
    REQUIRE(query != nullptr);

    // Create the $target query param. (This happens to be equal to the vector in rec-0010.)
    // Same target as used by test "Query Vector Index"
    Query::Options options = optionsWithTargetVector(kTargetVector, kString);

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

    string queryStr = R"(SELECT META(a).id, META(other).id, APPROX_VECTOR_DISTANCE(a.vector, $target) )"
                      R"( FROM )"s
                      + collectionName
                      + R"( AS a JOIN other ON META(a).id = other.refID )"
                        R"( WHERE MATCH(other.sentence, "search") )"
                        R"( ORDER BY APPROX_VECTOR_DISTANCE(a.vector, $target) )";

    Retained<Query> query{store->compileQuery(queryStr, QueryLanguage::kN1QL)};
    REQUIRE(query != nullptr);

    // Create the $target query param. (This happens to be equal to the vector in rec-0010.)
    // Same target as used by test "Query Vector Index"
    Query::Options options = optionsWithTargetVector(kTargetVector, kData);

    // Run the query:
    Retained<QueryEnumerator> e(query->createEnumerator(&options));
    REQUIRE(e->getRowCount() == 4);

    // VECTOR_MATCH will fetch these docs: {"rec-0010", "rec-0031", "rec-0022", "rec-0012", "rec-0020"}
    // FTS MATCH will fetch {"doc02", "doc03", "doc01", "doc05"}
    // "doc03" does not refer to any in result of VECTOR_MATCH.
    static constexpr slice expectedID1s[] = {"rec-0031", "rec-0022", "rec-0012", "rec-0011"};
    static constexpr slice expectedID2s[] = {"doc01", "doc05", "doc03", "doc02"};
    static constexpr float expectedDist[] = {4172, 10549, 29275, 121566};

    size_t i;
    for ( i = 0; e->next(); ++i ) {
        slice id1  = e->columns()[0]->asString();
        slice id2  = e->columns()[1]->asString();
        float dist = e->columns()[2]->asFloat();
#    if 0
        Log("id1 = %.*s, id2 = %.*s", FMTSLICE(id1), FMTSLICE(id2));
#    else
        CHECK(id1 == expectedID1s[i]);
        CHECK(id2 == expectedID2s[i]);
        CHECK_distances(dist, expectedDist[i]);
#    endif
    }
    CHECK(i == 4);
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

    string queryStr = R"(SELECT META(a).id, APPROX_VECTOR_DISTANCE(a.vector, $target) AS distance, a.sentence FROM )"s
                      + collectionName;
    queryStr += R"( AS a WHERE MATCH(a.sentence, "search") ORDER BY distance LIMIT 4)";

    Retained<Query> query{store->compileQuery(queryStr, QueryLanguage::kN1QL)};
    REQUIRE(query != nullptr);

    Query::Options options = optionsWithTargetVector(kTargetVector, kString);

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

static pair<string, string> splitCollectionName(const string& input) {
    // This system of randomizing REALLY messes with this test...
    if ( input == "_" ) { return {"_default", "_default"}; }

    auto dotPos = input.find('.');
    if ( dotPos == string::npos ) { return {"_default", input}; }

    return {input.substr(0, dotPos), input.substr(dotPos + 1)};
}

// This is a test for the functionality of the c4index_isTrained API.  The overall flow
// of the test is as follows:
//
// 1. Test that if there are not enough docs to perform the training, the index
//    remains untrained.
// 2. Test that creating an index, and then adding a sufficient number of docs
//    causes the index to become trained
// 3. Test that adding a sufficient number of docs, and then creating an index
//    causes the index to become trained
//
// During execution a few other things are also checked, such as a nonexistent index,
// and an index that is not a vector index, to make sure that exceptions are thrown as
// they should be.
//
// As a note, the timing of the training differs between 2 and 3 above currently.  Scenario
// 2 trains the index at doc write time, and scenario 3 trains at first query time.  This
// may change based on usability concerns.
TEST_CASE_METHOD(SIFTVectorQueryTest, "Index isTrained API", "[Query][.VectorSearch]") {
    bool expectedTrained{false};
    bool expectedPretrained{false};

    // Undo this silliness, I'm not spending the effort to find out the name it really wants
    // which is LiteCore_Tests_<random number> or something
    if ( collectionName == "db" ) collectionName = "_";

    // N_WAY_TEST_CASE_METHOD is not compatible with section, so redo all the
    // extra collections here

    SECTION("Insufficient docs") {
        SECTION("As-is") {}
        SECTION("Default scope") {
            collectionName = "Secondary";
            store          = &db->getKeyStore(string(".") + collectionName);
        }
        SECTION("Custom scope / collection") {
            collectionName = "scopey.subsidiary";
            store          = &db->getKeyStore(string(".") + collectionName);
        }

        expectedTrained    = false;
        expectedPretrained = false;
        createVectorIndex();
        readVectorDocs(100);
    }

    SECTION("Sufficient docs, index first") {
        SECTION("As-is") {}
        SECTION("Default scope") {
            collectionName = "Secondary";
            store          = &db->getKeyStore(string(".") + collectionName);
        }
        SECTION("Custom scope / collection") {
            collectionName = "scopey.subsidiary";
            store          = &db->getKeyStore(string(".") + collectionName);
        }

        expectedTrained    = true;
        expectedPretrained = true;
        createVectorIndex();
        readVectorDocs(256 * 30);
    }

    SECTION("Sufficient docs, load first") {
        SECTION("As-is") {}
        SECTION("Default scope") {
            collectionName = "Secondary";
            store          = &db->getKeyStore(string(".") + collectionName);
        }
        SECTION("Custom scope / collection") {
            collectionName = "scopey.subsidiary";
            store          = &db->getKeyStore(string(".") + collectionName);
        }

        expectedTrained    = true;
        expectedPretrained = false;
        readVectorDocs(256 * 30);
        createVectorIndex();
    }

    store->createIndex("sentence", "[[\".sentence\"]]", IndexSpec::kFullText, IndexSpec::FTSOptions{"english", true});

    auto              dbPath = db->filePath().dir();
    auto              parts  = FilePath::splitPath(dbPath.path().substr(0, dbPath.path().size() - 1));
    C4DatabaseConfig2 dbConfig{slice(parts.first), kC4DB_Create};

    auto                       fileNameParts = FilePath::splitExtension(parts.second);
    auto                       database  = C4Database::openNamed(FilePath(fileNameParts.first).fileName(), dbConfig);
    auto                       collParts = splitCollectionName(collectionName);
    C4Database::CollectionSpec collSpec(collParts.second, collParts.first);
    auto                       collection = database->createCollection(collSpec);
    REQUIRE(collection);

    {
        ExpectingExceptions e;
        try {
            collection->isIndexTrained("nonexistent"_sl);
            FAIL("No exception throw for non-existent collection");
        } catch ( error& e ) { CHECK(e == error::NoSuchIndex); }

        try {
            collection->isIndexTrained("sentence"_sl);
            FAIL("No exception throw for invalid collection type");
        } catch ( error& e ) { CHECK(e == error::InvalidParameter); }
    }

    bool isTrained = collection->isIndexTrained("vecIndex"_sl);
    CHECK(isTrained == expectedPretrained);

    // Need to run an arbitrary query to actually train the index
    string queryStr = R"(SELECT APPROX_VECTOR_DISTANCE(vector, $target) FROM )"s + collectionName + R"( LIMIT 5 )";

    Retained<Query> query{store->compileQuery(queryStr, QueryLanguage::kN1QL)};

    Encoder enc;
    enc.beginDictionary();
    enc.writeKey("target");
    enc.writeData(slice(kTargetVector, sizeof(kTargetVector)));
    enc.endDictionary();
    Query::Options            options(enc.finish());
    Retained<QueryEnumerator> e(query->createEnumerator(&options));

    isTrained = collection->isIndexTrained("vecIndex"_sl);
    CHECK(isTrained == expectedTrained);
    if ( !isTrained ) ++expectedWarningsLogged;  // "Untrained index; queries may be slow."
}

TEST_CASE_METHOD(SIFTVectorQueryTest, "enableExtension API", "[.VectorSearch]") {
    ExpectingExceptions e;
    C4Error             err;
    auto                result = c4_enableExtension("BadName"_sl, FLStr(sExtensionPath.c_str()), &err);
    CHECK(!result);
    CHECK(err.code == kC4ErrorInvalidParameter);
}

N_WAY_TEST_CASE_METHOD(SIFTVectorQueryTest, "Inspect Vector Index", "[Query][.VectorSearch]") {
    auto allKeyStores = db->allKeyStoreNames();
    readVectorDocs(100);
    createVectorIndex();

    std::vector<float> vec(128);
    auto               doc = inspectVectorIndex("vecIndex");
    for ( ArrayIterator iter(doc->asArray()); iter; ++iter ) {
        auto    row    = iter.value()->asArray();
        slice   key    = row->get(0)->asString();
        slice   rawVec = row->get(1)->asData();
        int64_t bucket = row->get(2)->asInt();
        REQUIRE(rawVec.size == 128 * sizeof(float));
#    if 1
        memcpy(vec.data(), rawVec.buf, rawVec.size);
        std::cerr << key << " (" << bucket << ") = [";
        for ( size_t i = 0; i < 128; ++i ) std::cerr << vec[i] << ' ';
        std::cerr << ']' << std::endl;
#    endif
    }
    CHECK(doc->asArray()->count() == 100);
}

TEST_CASE_METHOD(SIFTVectorQueryTest, "APPROX_VECTOR_DISTANCE Errors (Bad Metric)", "[.VectorSearch]") {
    IndexSpec::VectorOptions opts =
            IndexSpec::VectorOptions(128, vectorsearch::FlatClustering{256}, IndexSpec::DefaultEncoding);
    string pre = R"(SELECT META(a).id FROM )"s + collectionName
                 + R"( AS a ORDER BY APPROX_VECTOR_DISTANCE(a.vector, $target)";
    string          post = R"() LIMIT 5)";
    Retained<Query> query;

    // c.f. kMetricNames in VectorIndexSpec.cc
    const char*        kMetrics[]   = {"euclidean",
                                       "L2",
                                       "euclidean2",
                                       "L2_squared",
                                       "euclidean_squared",
                                       "cosine",  //5
                                       "dot",
                                       "cosine_distance",
                                       "cosine_similarity",
                                       "dot_product_distance",
                                       "dot_product_similarity",  //10
                                       "default"};
    constexpr unsigned kMetricCount = (unsigned)sizeof(kMetrics) / sizeof(char*);

    SECTION("Default Metric") {
        // Metric is not specified in the following index.
        VectorQueryTest::createVectorIndex("vecIndex", "[ ['.vector'] ]", opts);
        for ( unsigned i = 0; i < kMetricCount; ++i ) {
            string queryStr = pre + ", '" + kMetrics[i] + "'" + post;
            switch ( i ) {
                case 2:
                case 3:
                case 4:
                case 11:
                    // These metrics match the index with the default metric.
                    query = store->compileQuery(queryStr, QueryLanguage::kN1QL);
                    CHECK(query != nullptr);
                    break;
                default:
                    {
                        ExpectingExceptions e;
                        try {
                            query = store->compileQuery(queryStr, QueryLanguage::kN1QL);
                        } catch ( error& err ) {
                            CHECK(err.domain == error::LiteCore);
                            CHECK(err.code == error::InvalidQuery);
                            CHECK("in 3rd argument to APPROX_VECTOR_DISTANCE, "s + kMetrics[i]
                                          + " does not match the index's metric, euclidean2"s
                                  == err.what());
                        }
                        CHECK(query == nullptr);
                    }
                    break;
            }
            query = nullptr;
        }
    }

    SECTION("Non-default Metric") {
        // vectorsearch::Metric -> compatible metric names in kMetrics
        const std::vector<unsigned> kCompatibles[] = {
                {2, 3, 4, 11},  // Euclidean2 ->  "euclidean2", "L2_squared", "euclidean_squared", "default"
                {5, 7},         // CosineDistance -> "cosine", "cosine_distance"
                {0, 1},         // Euclidean -> "euclidean", "L2"
                {8},            // CosineSimilarity -> "cosine_similarity"
                {6, 9},         // DotProductDistance -> "dot", "dot_product_distance"
                {10}            // DotProductSimilarity -> "dot_product_similarity"
        };

        for ( unsigned m = 0; m <= (unsigned)vectorsearch::Metric::MaxValue; ++m ) {
            opts.metric = (vectorsearch::Metric)m;
            // Explicitly assign metric to the index
            VectorQueryTest::createVectorIndex("vecIndex", "[ ['.vector'] ]", opts);
            for ( unsigned i = 0; i < kMetricCount; ++i ) {
                string queryStr   = pre + ", '" + kMetrics[i] + "'" + post;
                bool   compatible = false;
                for ( auto c : kCompatibles[m] ) {
                    if ( c == i ) {
                        compatible = true;
                        break;
                    }
                }
                if ( compatible ) {
                    query = store->compileQuery(queryStr, QueryLanguage::kN1QL);
                    CHECK(query != nullptr);
                } else {
                    ExpectingExceptions e;
                    try {
                        query = store->compileQuery(queryStr, QueryLanguage::kN1QL);
                    } catch ( error& err ) {
                        CHECK(err.domain == error::LiteCore);
                        CHECK(err.code == error::InvalidQuery);
                        CHECK("in 3rd argument to APPROX_VECTOR_DISTANCE, "s + kMetrics[i]
                                      + " does not match the index's metric, "
                                      + string(vectorsearch::NameOfMetric(opts.metric))
                              == err.what());
                    }
                    CHECK(query == nullptr);
                }
                query = nullptr;
            }
        }
    }
}

TEST_CASE_METHOD(SIFTVectorQueryTest, "APPROX_VECTOR_DISTANCE Errors (Misc)", "[.VectorSearch]") {
    IndexSpec::VectorOptions opts =
            IndexSpec::VectorOptions(128, vectorsearch::FlatClustering{256}, IndexSpec::DefaultEncoding);
    VectorQueryTest::createVectorIndex("vecIndex", "[ ['.vector'] ]", opts);
    Retained<Query> query;

    SECTION("Not Indexed") {
        // "book" is not indexed
        string queryStr = R"(SELECT META(a).id FROM )"s + collectionName
                          + R"( AS a ORDER BY APPROX_VECTOR_DISTANCE(a.book, $target) LIMIT 5)";
        {
            ExpectingExceptions e;
            try {
                query = store->compileQuery(queryStr, QueryLanguage::kN1QL);
            } catch ( error& err ) {
                CHECK(err.domain == error::LiteCore);
                CHECK(err.code == error::NoSuchIndex);
                CHECK("vector search with APPROX_VECTOR_DISTANCE requires a vector index on [\".book\"]"s
                      == err.what());
            }
            CHECK(query == nullptr);
        }
    }

    SECTION("Unsupported accurate") {
        // accurate is the 5th argument to APPROX_VECTOR_DISTNCE
        // only false is supported.
        string queryStr =
                R"(SELECT META(a).id FROM )"s + collectionName
                + R"( AS a ORDER BY APPROX_VECTOR_DISTANCE(a.vector, $target, 'euclidean2', 1, false) LIMIT 5)";
        query = store->compileQuery(queryStr, QueryLanguage::kN1QL);
        CHECK(query != nullptr);

        query = nullptr;
        {
            queryStr = R"(SELECT META(a).id FROM )"s + collectionName
                       + R"( AS a ORDER BY APPROX_VECTOR_DISTANCE(a.vector, $target, 'euclidean2', 1, true) LIMIT 5)";
            ExpectingExceptions e;
            try {
                query = store->compileQuery(queryStr, QueryLanguage::kN1QL);
            } catch ( error& err ) {
                CHECK(err.domain == error::LiteCore);
                CHECK(err.code == error::InvalidQuery);
                CHECK("APPROX_VECTOR_DISTANCE does not support 'accurate'=true"s == err.what());
            }
            CHECK(query == nullptr);
        }
        // WARNING Invalid LiteCore query: APPROX_VECTOR_DISTANCE does not support 'accurate'=true
        expectedWarningsLogged = 1;
    }

    SECTION("Invalid nprobes") {
        // nprobes, the 4th argument to APPROX_VECTOR_DISTANCE must be greater than 0
        string queryStr = R"(SELECT META(a).id FROM )"s + collectionName
                          + R"( AS a ORDER BY APPROX_VECTOR_DISTANCE(a.vector, $target, 'euclidean2', 1) LIMIT 5)";
        query = store->compileQuery(queryStr, QueryLanguage::kN1QL);
        CHECK(query != nullptr);

        query = nullptr;
        {
            for ( int count = 0; count < 2; ++count ) {
                if ( count == 0 )
                    queryStr = R"(SELECT META(a).id FROM )"s + collectionName
                               + R"( AS a ORDER BY APPROX_VECTOR_DISTANCE(a.vector, $target, 'euclidean2', 0) LIMIT 5)";
                else
                    queryStr =
                            R"(SELECT META(a).id FROM )"s + collectionName
                            + R"( AS a ORDER BY APPROX_VECTOR_DISTANCE(a.vector, $target, 'euclidean2', -6) LIMIT 5)";
                ExpectingExceptions e;
                try {
                    query = store->compileQuery(queryStr, QueryLanguage::kN1QL);
                } catch ( error& err ) {
                    CHECK(err.domain == error::LiteCore);
                    CHECK(err.code == error::InvalidQuery);
                    CHECK("4th argument (numProbes) to APPROX_VECTOR_DISTANCE must be a positive integer"s
                          == err.what());
                }
                CHECK(query == nullptr);
                // Default WARNING Invalid LiteCore query: 4th argument (numProbes) to APPROX_VECTOR_DISTANCE must be a positive integer
                ++expectedWarningsLogged;
            }
        }
    }

    SECTION("APPROX_VECTOR_DISTANCE with OR") {
        string queryStr = R"(SELECT META(a).id FROM )"s + collectionName
                          + R"( AS a WHERE APPROX_VECTOR_DISTANCE(a.vector, $target))";
        // This is a hybrid query and we don't need LIMIT clause.
        query = store->compileQuery(queryStr, QueryLanguage::kN1QL);
        CHECK(query != nullptr);

        query = nullptr;
        {
            queryStr = R"(SELECT META(a).id FROM )"s + collectionName
                       + R"( AS a WHERE APPROX_VECTOR_DISTANCE(a.vector, $target) OR a.title = 'couchbase')";
            ExpectingExceptions e;
            try {
                query = store->compileQuery(queryStr, QueryLanguage::kN1QL);
            } catch ( error& err ) {
                CHECK(err.domain == error::LiteCore);
                CHECK(err.code == error::InvalidQuery);
                CHECK("APPROX_VECTOR_DISTANCE can't be used within an OR in a WHERE clause"s == err.what());
            }
            CHECK(query == nullptr);
        }
        // WARNING Invalid LiteCore query: APPROX_VECTOR_DISTANCE can't be used within an OR in a WHERE clause
        expectedWarningsLogged = 1;

        {
            // This is non-hybrid
            queryStr =
                    R"(SELECT META(a).id FROM )"s + collectionName
                    + R"( AS a WHERE APPROX_VECTOR_DISTANCE(a.vector, $target) < 0.5 OR a.title = 'couchbase' LIMIT 5)";
            ExpectingExceptions e;
            try {
                query = store->compileQuery(queryStr, QueryLanguage::kN1QL);
            } catch ( error& err ) {
                CHECK(err.domain == error::LiteCore);
                CHECK(err.code == error::InvalidQuery);
                CHECK("APPROX_VECTOR_DISTANCE can't be used within an OR in a WHERE clause"s == err.what());
            }
            CHECK(query == nullptr);
        }
        expectedWarningsLogged++;
    }

    SECTION("Invalid First Argument") {
        // First argument must be an expression evaluate to a property in the databas instead of
        // the name of the index.
        string queryStr = R"(SELECT META(a).id FROM )"s + collectionName
                          + R"( AS a ORDER BY APPROX_VECTOR_DISTANCE('vecIndex', $target) LIMIT 5)";
        {
            ExpectingExceptions e;
            try {
                query = store->compileQuery(queryStr, QueryLanguage::kN1QL);
            } catch ( error& err ) {
                CHECK(err.domain == error::LiteCore);
                CHECK(err.code == error::NoSuchIndex);
                CHECK("vector search with APPROX_VECTOR_DISTANCE requires a vector index on \"vecIndex\""s
                      == err.what());
            }
            CHECK(query == nullptr);
        }
    }
}

TEST_CASE_METHOD(SIFTVectorQueryTest, "APPROX_VECTOR_DISTANCE Errors (Non-Hybrid without Limit)", "[.VectorSearch]") {
    IndexSpec::VectorOptions opts =
            IndexSpec::VectorOptions(128, vectorsearch::FlatClustering{256}, IndexSpec::DefaultEncoding);
    VectorQueryTest::createVectorIndex("vecIndex", "[ ['.vector'] ]", opts);
    Retained<Query> query;

    // WARNING Invalid LiteCore query: a LIMIT must be given when using APPROX_VECTOR_DIST()
    expectedWarningsLogged = 1;

    SECTION("APPROX_VECTOR_DIST in ORDER BY") {
        string queryStr = R"(SELECT META(a).id FROM )"s + collectionName
                          + R"( AS a ORDER BY APPROX_VECTOR_DISTANCE(a.vector, $target))";
        // working with LIMIT
        query = store->compileQuery(queryStr + " LIMIT 5", QueryLanguage::kN1QL);
        CHECK(query != nullptr);

        query = nullptr;
        {
            ExpectingExceptions e;
            try {
                query = store->compileQuery(queryStr, QueryLanguage::kN1QL);
            } catch ( error& err ) {
                CHECK(err.domain == error::LiteCore);
                CHECK(err.code == error::InvalidQuery);
                CHECK("a LIMIT must be given when using APPROX_VECTOR_DISTANCE()"s == err.what());
            }
            CHECK(query == nullptr);
        }
    }

    SECTION("APPROX_VECTOR_DIST in WHERE") {
        string queryStr = R"(SELECT META(a).id FROM )"s + collectionName
                          + R"( AS a WHERE APPROX_VECTOR_DISTANCE(a.vector, $target) < 0.5)";
        // This is non-hybrid, requires LIMIT
        query = store->compileQuery(queryStr + " LIMIT 5", QueryLanguage::kN1QL);
        CHECK(query != nullptr);

        query = nullptr;
        {
            ExpectingExceptions e;
            try {
                query = store->compileQuery(queryStr, QueryLanguage::kN1QL);
            } catch ( error& err ) {
                CHECK(err.domain == error::LiteCore);
                CHECK(err.code == error::InvalidQuery);
                CHECK("a LIMIT must be given when using APPROX_VECTOR_DISTANCE()"s == err.what());
            }
        }
    }

    SECTION("APPROX_VECTOR_DIST in SELECT") {
        string queryStr = R"(SELECT META(a).id, APPROX_VECTOR_DISTANCE(a.vector, $target) AS distant FROM )"s
                          + collectionName + " AS a";
        query = store->compileQuery(queryStr + " LIMIT 5", QueryLanguage::kN1QL);
        CHECK(query != nullptr);

        query = nullptr;
        {
            ExpectingExceptions e;
            try {
                query = store->compileQuery(queryStr, QueryLanguage::kN1QL);
            } catch ( error& err ) {
                CHECK(err.domain == error::LiteCore);
                CHECK(err.code == error::InvalidQuery);
                CHECK("a LIMIT must be given when using APPROX_VECTOR_DISTANCE()"s == err.what());
            }
            CHECK(query == nullptr);
        }
    }
}
#endif
