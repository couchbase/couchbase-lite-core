//
//  c4AllDocsPerformanceTest.cc
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 11/16/15.
//  Copyright (c) 2015-2016 Couchbase. All rights reserved.
//

#include "c4Test.hh"
#ifdef _MSC_VER
#include <chrono>
#endif

static const size_t kSizeOfDocument = 1000;
static const unsigned kNumDocuments = 100000;


class C4AllDocsPerformanceTest : public C4Test {
public:

    C4AllDocsPerformanceTest(int testOption)
    :C4Test(testOption)
    {
        char content[kSizeOfDocument];
        memset(content, 'a', sizeof(content)-1);
        content[sizeof(content)-1] = 0;

        
        C4Error error;
        REQUIRE(c4db_beginTransaction(db, &error));

        for (unsigned i = 0; i < kNumDocuments; i++) {
            char docID[50];
            sprintf(docID, "doc-%08lx-%08lx-%08lx-%04x", random(), random(), random(), i);
            char revID[50];
            sprintf(revID, "1-deadbeefcafebabe80081e50");
            char json[kSizeOfDocument+100];
            sprintf(json, "{\"content\":\"%s\"}", content);

            C4Slice history[1] = {c4str(revID)};
            C4DocPutRequest rq = {};
            rq.existingRevision = true;
            rq.docID = c4str(docID);
            rq.history = history;
            rq.historyCount = 1;
            rq.body = c4str(json);
            rq.save = true;
            auto doc = c4doc_put(db, &rq, NULL, &error);
            REQUIRE(doc);
            c4doc_free(doc);
        }

        REQUIRE(c4db_endTransaction(db, true, &error));
        fprintf(stderr, "Created %u docs\n", kNumDocuments);

        REQUIRE(c4db_getDocumentCount(db) == (uint64_t)kNumDocuments);
    }
};


N_WAY_TEST_CASE_METHOD(C4AllDocsPerformanceTest, "AllDocsPerformance", "[Perf][.slow][C]") {
    auto start = clock();

    C4EnumeratorOptions options = kC4DefaultEnumeratorOptions;
    options.flags &= ~kC4IncludeBodies;
    C4Error error;
    auto e = c4db_enumerateAllDocs(db, kC4SliceNull, kC4SliceNull, &options, &error);
    REQUIRE(e);
    C4Document* doc;
    unsigned i = 0;
    while (NULL != (doc = c4enum_nextDocument(e, &error))) {
        i++;
        c4doc_free(doc);
    }
    c4enum_free(e);
    REQUIRE(i == kNumDocuments);

    double elapsed = (clock() - start) / (double)CLOCKS_PER_SEC;
    fprintf(stderr, "Enumerating %u docs took %.3f ms (%.3f ms/doc)\n",
            i, elapsed*1000.0, elapsed/i*1000.0);
}
