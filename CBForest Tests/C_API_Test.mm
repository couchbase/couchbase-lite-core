//
//  C_API_Test.mm
//  CBForest
//
//  Created by Jens Alfke on 11/16/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//

#import <XCTest/XCTest.h>
#import "testutil.h"
#import "c4Database.h"
#import "c4Document.h"
#import "c4DocEnumerator.h"


@interface C_API_Test : XCTestCase
@end


@implementation C_API_Test
{
    C4Database *db;
}

static const size_t kSizeOfDocument = 1000;
static const unsigned kNumDocuments = 10000;

- (void)setUp {
    [super setUp];

    const char *dbPath = "/tmp/forest_temp.fdb";
    ::unlink("/tmp/forest_temp.fdb");
    ::unlink("/tmp/forest_temp.fdb.0");
    ::unlink("/tmp/forest_temp.fdb.1");
    ::unlink("/tmp/forest_temp.fdb.meta");

    C4Error error;
    db = c4db_open(c4str(dbPath), kC4DB_Create, NULL, &error);
    Assert(db != NULL);
}

- (void)tearDown {
    if (db)
        c4db_delete(db, NULL);
    [super tearDown];
}

- (void)testExample {
    // This is an example of a functional test case.
    // Use XCTAssert and related functions to verify your tests produce the correct results.
}

- (void)testPerformanceExample {
    char content[kSizeOfDocument];
    memset(content, 'a', sizeof(content)-1);
    content[sizeof(content)-1] = 0;

    // Create documents:
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
        AssertEq(revs, 1);
        Assert(c4doc_save(doc, 20, &error));
        c4doc_free(doc);
    }

    Assert(c4db_endTransaction(db, true, &error));
    AssertEq(c4db_getDocumentCount(db), (uint64_t)kNumDocuments);
    fprintf(stderr, "Created %u docs\n", kNumDocuments);

    [self measureBlock:^{
        C4EnumeratorOptions options = kC4DefaultEnumeratorOptions;
        options.flags &= ~kC4IncludeBodies;
        C4Error enumError;
        auto e = c4db_enumerateAllDocs(db, kC4SliceNull, kC4SliceNull, &options, &enumError);
        Assert(e);
        C4Document* doc;
        unsigned i = 0;
        while (NULL != (doc = c4enum_nextDocument(e, &enumError))) {
            i++;
            c4doc_free(doc);
        }
        c4enum_free(e);
        Assert(i == kNumDocuments);
    }];
}

@end
