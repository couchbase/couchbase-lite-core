//
//  c4AllDocsPerformanceTest.cpp
//  CBForest
//
//  Created by Jens Alfke on 11/16/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//

#include "c4Test.hh"
#ifdef _MSC_VER
#define random() rand()
#include <chrono>
#endif

class C4AllDocsPerformanceTest : public C4Test {
public:

    static const size_t kSizeOfDocument = 1000;
    static const unsigned kNumDocuments = 100000;

    void setUp() {
        C4Test::setUp();

        char content[kSizeOfDocument];
        memset(content, 'a', sizeof(content)-1);
        content[sizeof(content)-1] = 0;

        
        C4Error error;
        Assert(c4db_beginTransaction(db, &error));

        for (unsigned i = 0; i < kNumDocuments; i++) {
            char docID[50];
            sprintf(docID, "doc-%08lx-%08lx-%08lx-%04x", random(), random(), random(), i);
            C4Document* doc = c4doc_get(db, c4str(docID), false, &error);
            Assert(doc);
            char revID[50];
            sprintf(revID, "1-deadbeefcafebabe80081e50");
            char json[kSizeOfDocument+100];
            sprintf(json, "{\"content\":\"%s\"}", content);
            int revs = c4doc_insertRevision(doc, c4str(revID), c4str(json), false, false, false, &error);
            AssertEqual(revs, 1);
            Assert(c4doc_save(doc, 20, &error));
            c4doc_free(doc);
        }

        Assert(c4db_endTransaction(db, true, &error));
        fprintf(stderr, "Created %u docs\n", kNumDocuments);

        AssertEqual(c4db_getDocumentCount(db), (uint64_t)kNumDocuments);
    }

    void testAllDocsPerformance() {
        auto start = clock();

        C4EnumeratorOptions options = kC4DefaultEnumeratorOptions;
        options.flags &= ~kC4IncludeBodies;
        C4Error error;
        auto e = c4db_enumerateAllDocs(db, kC4SliceNull, kC4SliceNull, &options, &error);
        Assert(e);
        C4Document* doc;
        unsigned i = 0;
        while (NULL != (doc = c4enum_nextDocument(e, &error))) {
            i++;
            c4doc_free(doc);
        }
        c4enum_free(e);
        Assert(i == kNumDocuments);

        double elapsed = (clock() - start) / (double)CLOCKS_PER_SEC;
        fprintf(stderr, "Enumerating %u docs took %.3f ms (%.3f ms/doc)\n",
                i, elapsed*1000.0, elapsed/i*1000.0);
    }

    CPPUNIT_TEST_SUITE( C4AllDocsPerformanceTest );
    CPPUNIT_TEST( testAllDocsPerformance );
    CPPUNIT_TEST_SUITE_END();
};

//CPPUNIT_TEST_SUITE_REGISTRATION(C4AllDocsPerformanceTest);
