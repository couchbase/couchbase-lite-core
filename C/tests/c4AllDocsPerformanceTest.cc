//
// c4AllDocsPerformanceTest.cc
//
// Copyright (c) 2015 Couchbase, Inc All rights reserved.
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

#include "c4Test.hh"
#include "c4DocEnumerator.h"
#include "Benchmark.hh"
#include "SecureRandomize.hh"
#include <chrono>

static const size_t kSizeOfDocument = 1000;
static const unsigned kNumDocuments = 100000;

static C4Document* c4enum_nextDocument(C4DocEnumerator *e, C4Error *outError) noexcept {
    return c4enum_next(e, outError) ? c4enum_getDocument(e, outError) : nullptr;
}


class C4AllDocsPerformanceTest : public C4Test {
public:

    C4AllDocsPerformanceTest(int testOption)
    :C4Test(testOption)
    {
        char content[kSizeOfDocument];
        memset(content, 'a', sizeof(content)-1);
        content[sizeof(content)-1] = 0;

        
        C4Error error;
        REQUIRE(c4db_beginTransaction(db, WITH_ERROR(&error)));

        for (unsigned i = 0; i < kNumDocuments; i++) {
            char docID[50];
            sprintf(docID, "doc-%08x-%08x-%08x-%04x", litecore::RandomNumber(), litecore::RandomNumber(), litecore::RandomNumber(), i);
            char revID[50];
            sprintf(revID, "1-deadbeefcafebabe80081e50");
            char json[kSizeOfDocument+100];
            sprintf(json, "{\"content\":\"%s\"}", content);
            C4SliceResult body = c4db_encodeJSON(db, c4str(json), ERROR_INFO(error));
            REQUIRE(body.buf);

            C4Slice history[1] = {isRevTrees() ? c4str("1-deadbeefcafebabe80081e50")
                                               : c4str("1@deadbeefcafebabe80081e50")};
            C4DocPutRequest rq = {};
            rq.existingRevision = true;
            rq.docID = c4str(docID);
            rq.history = history;
            rq.historyCount = 1;
            rq.body = (C4Slice)body;
            rq.save = true;
            auto doc = c4doc_put(db, &rq, nullptr, ERROR_INFO(error));
            REQUIRE(doc);
            c4doc_release(doc);
            c4slice_free(body);
        }

        REQUIRE(c4db_endTransaction(db, true, WITH_ERROR(&error)));
        C4Log("Created %u docs", kNumDocuments);

        REQUIRE(c4db_getDocumentCount(db) == (uint64_t)kNumDocuments);
    }
};


N_WAY_TEST_CASE_METHOD(C4AllDocsPerformanceTest, "AllDocsPerformance", "[Perf][.slow][C]") {
    fleece::Stopwatch st;

    C4EnumeratorOptions options = kC4DefaultEnumeratorOptions;
    options.flags &= ~kC4IncludeBodies;
    C4Error error;
    auto e = c4db_enumerateAllDocs(db, &options, ERROR_INFO(error));
    REQUIRE(e);
    C4Document* doc;
    unsigned i = 0;
    while (nullptr != (doc = c4enum_nextDocument(e, ERROR_INFO(error)))) {
        i++;
        c4doc_release(doc);
    }
    c4enum_free(e);
    REQUIRE(i == kNumDocuments);

    double elapsed = st.elapsedMS();
    C4Log("Enumerating %u docs took %.3f ms (%.3f ms/doc)", i, elapsed, elapsed/i);
}
