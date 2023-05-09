//
// c4AllDocsPerformanceTest.cc
//
// Copyright 2015-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "c4Test.hh"  // IWYU pragma: keep
#include "c4Collection.h"
#include "c4DocEnumerator.h"
#include "Stopwatch.hh"
#include "SecureRandomize.hh"
#include <chrono>

static constexpr size_t   kSizeOfDocument = 1000;
static constexpr unsigned kNumDocuments   = 100000;

static C4Document* c4enum_nextDocument(C4DocEnumerator* e, C4Error* outError) noexcept {
    return c4enum_next(e, outError) ? c4enum_getDocument(e, outError) : nullptr;
}

class C4AllDocsPerformanceTest : public C4Test {
  public:
    explicit C4AllDocsPerformanceTest(int testOption) : C4Test(testOption) {
        char content[kSizeOfDocument];
        memset(content, 'a', sizeof(content) - 1);
        content[sizeof(content) - 1] = 0;


        C4Error error;
        REQUIRE(c4db_beginTransaction(db, WITH_ERROR(&error)));

        constexpr size_t docBufSize = 50, revBufSize = 50, jsonBufSize = kSizeOfDocument + 100;

        for ( unsigned i = 0; i < kNumDocuments; i++ ) {
            char docID[docBufSize];
            snprintf(docID, docBufSize, "doc-%08x-%08x-%08x-%04x", litecore::RandomNumber(), litecore::RandomNumber(),
                     litecore::RandomNumber(), i);
            char revID[revBufSize];
            snprintf(revID, revBufSize, "1-deadbeefcafebabe80081e50");
            char json[jsonBufSize];
            snprintf(json, jsonBufSize, R"({"content":"%s"})", content);
            C4SliceResult body = c4db_encodeJSON(db, c4str(json), ERROR_INFO(error));
            REQUIRE(body.buf);

            C4Slice         history[1] = {isRevTrees() ? c4str("1-deadbeefcafebabe80081e50")
                                                       : c4str("1@deadbeefcafebabe80081e50")};
            C4DocPutRequest rq         = {};
            rq.existingRevision        = true;
            rq.docID                   = c4str(docID);
            rq.history                 = history;
            rq.historyCount            = 1;
            rq.body                    = (C4Slice)body;
            rq.save                    = true;
            auto defaultColl           = getCollection(db, kC4DefaultCollectionSpec);
            auto doc                   = c4coll_putDoc(defaultColl, &rq, nullptr, ERROR_INFO(error));
            REQUIRE(doc);
            c4doc_release(doc);
            c4slice_free(body);
        }

        REQUIRE(c4db_endTransaction(db, true, WITH_ERROR(&error)));
        C4Log("Created %u docs", kNumDocuments);

        auto defaultColl = getCollection(db, kC4DefaultCollectionSpec);
        REQUIRE(c4coll_getDocumentCount(defaultColl) == (uint64_t)kNumDocuments);
    }
};

N_WAY_TEST_CASE_METHOD(C4AllDocsPerformanceTest, "AllDocsPerformance", "[Perf][.slow][C]") {
    fleece::Stopwatch   st;
    auto                defaultColl = getCollection(db, kC4DefaultCollectionSpec);
    C4EnumeratorOptions options     = kC4DefaultEnumeratorOptions;
    options.flags &= ~kC4IncludeBodies;
    C4Error error;
    auto    e = c4coll_enumerateAllDocs(defaultColl, &options, ERROR_INFO(error));
    REQUIRE(e);
    C4Document* doc;
    unsigned    i = 0;
    while ( nullptr != (doc = c4enum_nextDocument(e, ERROR_INFO(error))) ) {
        i++;
        c4doc_release(doc);
    }
    c4enum_free(e);
    REQUIRE(i == kNumDocuments);

    double elapsed = st.elapsedMS();
    C4Log("Enumerating %u docs took %.3f ms (%.3f ms/doc)", i, elapsed, elapsed / i);
}
