//
// ArrayQueryTest.cc
//
// Copyright © 2024 Couchbase. All rights reserved.
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

#include "QueryTest.hh"

#define SKIP_ARRAY_INDEXES  // Array indexes aren't exposed in Couchbase Lite (yet?)

class ArrayQueryTest : public QueryTest {
  protected:
    Retained<Query> query;

    explicit ArrayQueryTest(int option) : QueryTest(option) {}

    void checkQuery(int docNo, int expectedRowCount) {
        Retained<QueryEnumerator> e(query->createEnumerator());
        CHECK(e->getRowCount() == expectedRowCount);
        while ( e->next() ) {
            auto   cols          = e->columns();
            slice  docID         = cols[0]->asString();
            string expectedDocID = stringWithFormat("rec-%03d", docNo);
            CHECK(docID == slice(expectedDocID));
            ++docNo;
        }
    }

    void testArrayQuery(const string& json, bool checkOptimization) {
        addArrayDocs(1, 90);

        query              = store->compileQuery(json);
        string explanation = query->explain();
        Log("%s", explanation.c_str());
        checkQuery(88, 3);

#ifndef SKIP_ARRAY_INDEXES
        Log("-------- Creating index --------");
        store->createIndex("numbersIndex"_sl, R"([[".numbers"]])", IndexSpec::kArray);
        Log("-------- Recompiling query with index --------");
        query = store->compileQuery(json);
        checkOptimized(query, checkOptimization);
        checkQuery(88, 3);

        Log("-------- Adding a doc --------");
        addArrayDocs(91, 1);
        checkQuery(88, 4);

        Log("-------- Purging a doc --------");
        deleteDoc("rec-091"_sl, true);
        checkQuery(88, 3);

        Log("-------- Soft-deleting a doc --------");
        deleteDoc("rec-090"_sl, false);
        checkQuery(88, 2);

        Log("-------- Un-deleting a doc --------");
        undeleteDoc("rec-090"_sl);
        checkQuery(88, 3);
#endif
    }
};

N_WAY_TEST_CASE_METHOD(ArrayQueryTest, "Query ANY", "[Query]") {
    testArrayQuery(json5("['SELECT', {\
                             WHERE: ['ANY', 'num', ['.numbers'],\
                                            ['=', ['?num'], 'eight-eight']]}]"),
                   false);
}

N_WAY_TEST_CASE_METHOD(ArrayQueryTest, "Query UNNEST", "[Query][Unnest]") {
    testArrayQuery(json5("['SELECT', {\
                              FROM: [{as: 'doc'}, \
                                     {as: 'num', 'unnest': ['.doc.numbers']}],\
                              WHERE: ['=', ['.num'], 'eight-eight']}]"),
                   true);
}

N_WAY_TEST_CASE_METHOD(ArrayQueryTest, "Query ANY expression", "[Query]") {
    addArrayDocs(1, 90);

    auto json          = json5("['SELECT', {\
                          WHERE: ['ANY', 'num', ['[]', ['.numbers[0]'], ['.numbers[1]']],\
                                         ['=', ['?num'], 'eight']]}]");
    query              = store->compileQuery(json);
    string explanation = query->explain();
    Log("%s", explanation.c_str());

    checkQuery(12, 2);
}

#if 0  // v4.0 does not support UNNEST of expression
N_WAY_TEST_CASE_METHOD(ArrayQueryTest, "Query UNNEST expression", "[Query][Unnest]") {
    addArrayDocs(1, 90);

    auto json          = json5("['SELECT', {\
                              FROM: [{as: 'doc'}, \
                                     {as: 'num', 'unnest': ['[]', ['.doc.numbers[0]'], ['.doc.numbers[1]']]}],\
                              WHERE: ['=', ['.num'], 'one-eight']}]");
    query              = store->compileQuery(json);
    string explanation = query->explain();
    Log("%s", explanation.c_str());

    checkQuery(22, 2);

#    ifndef SKIP_ARRAY_INDEXES
    if ( GENERATE(0, 1) ) {
        Log("-------- Creating JSON index --------");
        store->createIndex("numbersIndex"_sl, json5("[['[]', ['.numbers[0]'], ['.numbers[1]']]]"), IndexSpec::kArray);
    } else {
        Log("-------- Creating N1QL index --------");
        store->createIndex("numbersIndex"_sl, "[numbers[0], numbers[1]]", QueryLanguage::kN1QL, IndexSpec::kArray);
    }
    Log("-------- Recompiling query with index --------");
    query = store->compileQuery(json);
    checkOptimized(query);

    checkQuery(22, 2);
#    endif
}
#endif  // v4.0 does not support UNNEST of expression
