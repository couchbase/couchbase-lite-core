//
// QueryTest.hh
//
// Copyright 2018-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "DataFile.hh"
#include "Query.hh"
#include "FleeceImpl.hh"
#include "StringUtil.hh"

#include "LiteCoreTest.hh"
#include <algorithm>
#include <set>
#include <utility>
#include <vector>

using namespace litecore;
using namespace fleece::impl;

// NOTE: This test does not use RevTree or Database, so it stores plain Fleece in record bodies.


class QueryTest : public DataFileTestFixture {
  public:
    static const int numberOfOptions = 3;

    string collectionName;
    int    option{0};

  protected:
    QueryTest() : QueryTest(0) {}

    static unsigned alter2;
    static unsigned alter3;

    explicit QueryTest(int option) : option(option) {
        static const char* kSectionNames[3] = {"default collection", "other collection", "collection in other scope"};
        logSection(kSectionNames[option]);
        unsigned jump;
        switch ( option ) {
            case 0:
                jump           = alter3++ % 3;
                collectionName = (jump == 0) ? KeyStore::kDefaultCollectionName : (jump == 1) ? "_" : "cbl_core_temp";
                break;
            case 1:
                collectionName = (alter2++ % 2 == 0) ? "Secondary" : "_default.Secondary";
                store          = &db->getKeyStore(".Secondary");
                break;
            case 2:
                collectionName = "scopey.subsidiary";
                store          = &db->getKeyStore(".scopey.subsidiary");
                break;
            default:
                Assert(false, "Test option out of valid range");
        }
    }

    static void logSection(const string& name) { fprintf(stderr, "        --- %s\n", name.c_str()); }

    static string numberString(int n) {
        static const char* kDigit[10] = {"zero", "one", "two",   "three", "four",
                                         "five", "six", "seven", "eight", "nine"};
        string             str;
        do {
            str = string(kDigit[n % 10]) + str;
            n /= 10;
            if ( n > 0 ) str = string("-") + str;
        } while ( n > 0 );
        return str;
    }

    sequence_t writeNumberedDoc(int i, slice str, ExclusiveTransaction& t, DocumentFlags flags = DocumentFlags::kNone) {
        return writeDoc(slice(stringWithFormat("rec-%03d", i)), flags, t, [=](Encoder& enc) {
            enc.writeKey("num");
            enc.writeInt(i);
            enc.writeKey("type");
            enc.writeString("number");
            if ( str ) {
                enc.writeKey("str");
                enc.writeString(str);
            }
        });
    }

    // Write 100 docs with Fleece bodies of the form {"num":n} where n is the rec #
    void addNumberedDocs(int first = 1, int n = 100) {
        auto level = QueryLog.level();
        QueryLog.setLevel(LogLevel::Warning);
        ExclusiveTransaction t(store->dataFile());
        for ( int i = first; i < first + n; i++ ) REQUIRE(writeNumberedDoc(i, nullslice, t) == (sequence_t)i);
        t.commit();
        QueryLog.setLevel(level);
    }

    sequence_t writeArrayDoc(int i, ExclusiveTransaction& t, DocumentFlags flags = DocumentFlags::kNone) {
        return writeDoc(slice(stringWithFormat("rec-%03d", i)), flags, t, [=](Encoder& enc) {
            enc.writeKey("numbers");
            enc.beginArray();
            for ( int j = std::max(i - 5, 1); j <= i; j++ ) enc.writeString(numberString(j));
            enc.endArray();
            enc.writeKey("type");
            enc.writeString("array");
        });
    }

    void addArrayDocs(int first = 1, int n = 100) {
        ExclusiveTransaction t(store->dataFile());
        for ( int i = first; i < first + n; i++ ) REQUIRE(writeArrayDoc(i, t) == (sequence_t)i);
        t.commit();
    }

    void writeMultipleTypeDocs(ExclusiveTransaction& t) {
        writeDoc("doc1"_sl, DocumentFlags::kNone, t, [=](Encoder& enc) {
            enc.writeKey("value");
            enc.beginArray();
            enc.writeInt(1);
            enc.endArray();
        });

        writeDoc("doc2"_sl, DocumentFlags::kNone, t, [=](Encoder& enc) {
            enc.writeKey("value");
            enc.writeString("cool value");
        });

        writeDoc("doc3"_sl, DocumentFlags::kNone, t, [=](Encoder& enc) {
            enc.writeKey("value");
            enc.writeDouble(4.5);
        });

        writeDoc("doc4"_sl, DocumentFlags::kNone, t, [=](Encoder& enc) {
            enc.writeKey("value");
            enc.beginDictionary(1);
            enc.writeKey("subvalue");
            enc.writeString("FTW");
            enc.endDictionary();
        });

        writeDoc("doc5"_sl, DocumentFlags::kNone, t, [=](Encoder& enc) {
            enc.writeKey("value");
            enc.writeBool(true);
        });
    }

    void writeFalselyDocs(ExclusiveTransaction& t) {
        writeDoc("doc6"_sl, DocumentFlags::kNone, t, [=](Encoder& enc) {
            enc.writeKey("value");
            enc.beginArray();
            enc.endArray();
        });

        writeDoc("doc7"_sl, DocumentFlags::kNone, t, [=](Encoder& enc) {
            enc.writeKey("value");
            enc.beginDictionary();
            enc.endDictionary();
        });

        writeDoc("doc81"_sl, DocumentFlags::kNone, t, [=](Encoder& enc) {
            enc.writeKey("value");
            enc.writeBool(false);
        });
    }

    void deleteDoc(slice docID, bool hardDelete) {
        ExclusiveTransaction t(store->dataFile());
        if ( hardDelete ) {
            store->del(docID, t);
        } else {
            Record doc = store->get(docID);
            CHECK(doc.exists());
            doc.setFlag(DocumentFlags::kDeleted);
            store->set(doc, true, t);
        }
        t.commit();
    }

    void undeleteDoc(slice docID) {
        ExclusiveTransaction t(store->dataFile());
        Record               doc = store->get(docID);
        CHECK(doc.exists());
        doc.clearFlag(DocumentFlags::kDeleted);
        store->set(doc, true, t);
        t.commit();
    }

    static std::vector<std::string> extractIndexes(const std::vector<IndexSpec>& indexes) {
        std::set<std::string> retVal;
        for ( auto& i : indexes ) retVal.insert(i.name);
        return {retVal.begin(), retVal.end()};
    }

    int64_t rowsInQuery(const string& json) {
        Retained<Query>           query = store->compileQuery(json);
        Retained<QueryEnumerator> e(query->createEnumerator());
        return e->getRowCount();
    }

    void testExpressions(const std::vector<std::pair<std::string, std::string>>& tests) {
        {
            ExclusiveTransaction t(store->dataFile());
            writeNumberedDoc(1, nullslice, t);
            t.commit();
        }
        for ( auto& test : tests ) {
            INFO("Testing " << test.first);
            auto                      query = store->compileQuery(json5("{'WHAT': [" + test.first + "]}"));
            Retained<QueryEnumerator> e(query->createEnumerator());
            REQUIRE(e->getRowCount() == 1);
            REQUIRE(e->next());
            CHECK(e->columns()[0]->toString() == slice(test.second));
        }
    }

    void testExpressions(const std::vector<std::pair<std::string, int64_t>>& tests) {
        {
            ExclusiveTransaction t(store->dataFile());
            writeNumberedDoc(1, nullslice, t);
            t.commit();
        }
        for ( auto& test : tests ) {
            INFO("Testing " << test.first);
            auto                      query = store->compileQuery(json5("{'WHAT': [" + test.first + "]}"));
            Retained<QueryEnumerator> e(query->createEnumerator());
            REQUIRE(e->getRowCount() == 1);
            REQUIRE(e->next());
            CHECK(e->columns()[0]->asInt() == test.second);
        }
    }

    static void checkOptimized(Query* query, bool expectOptimized = true) {
        string explanation = query->explain();
        Log("Query:\n%s", explanation.c_str());
        auto optimized = (explanation.find("SCAN") == string::npos);
        CHECK(optimized == expectOptimized);
    }

    string queryWhat(const char* what) {
        auto                      query = store->compileQuery(json5(CONCAT("{'WHAT': [" << what << "]}")));
        Retained<QueryEnumerator> e(query->createEnumerator());
        REQUIRE(e->next());
        return e->columns()[0]->toJSONString();
    }
};
