//
//  Database_Test.mm
//  CBForest
//
//  Created by Jens Alfke on 5/13/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#import <XCTest/XCTest.h>
#import "testutil.h"
#import "Database.hh"
#import "DocEnumerator.hh"

using namespace forestdb;

#define kDBPath "/tmp/forest.db"


@interface Database_Test : XCTestCase
@end

@implementation Database_Test
{
    Database* db;
}

+ (void) initialize {
    if (self == [Database_Test class]) {
        LogLevel = kWarning;
    }
}

- (void)setUp
{
    ::unlink(kDBPath);
    [super setUp];
    db = new Database(kDBPath, FDB_OPEN_FLAG_CREATE, Database::defaultConfig());
}

- (void)tearDown
{
    delete db;
    [super tearDown];
}

- (void) invokeTest {
    try{
        [super invokeTest];
    } catch (error e) {
        XCTFail(@"Exception: %d", e.status);
    }
}

- (void) test01_DbInfo {
    auto info = db->getInfo();
    AssertEq(info.last_seqnum, 0);
    AssertEq(info.doc_count, 0);
    Assert(info.space_used > 0);
    Assert(info.file_size > 0);
}

- (void)test02_CreateDoc
{
    alloc_slice key("key");
    {
        Transaction t(db);
        t.set(key, nsstring_slice(@"value"));
    }
    Document doc = db->get(key);
    Assert(doc.key() == key);
    Assert(doc.body() == nsstring_slice(@"value"));
}

- (void) test03_SaveDocs {
    Transaction(db).set(nsstring_slice(@"a"), nsstring_slice(@"A"));   //WORKAROUND: Add a doc before the main transaction so it doesn't start at sequence 0

    Database aliased_db(kDBPath, 0, Database::defaultConfig());
    AssertEqual((NSString*)aliased_db.get(nsstring_slice(@"a")).body(), @"A");

    {
        Transaction t(db);
        Document doc(nsstring_slice(@"doc"));
        doc.setMeta(nsstring_slice(@"m-e-t-a"));
        doc.setBody(nsstring_slice(@"THIS IS THE BODY"));
        t.write(doc);

        AssertEq(doc.sequence(), 2);
        auto doc_alias = t.get(doc.sequence());
        Assert(doc_alias.key() == doc.key());
        Assert(doc_alias.meta() == doc.meta());
        Assert(doc_alias.body() == doc.body());

        doc_alias.setBody(nsstring_slice(@"NU BODY"));
        t.write(doc_alias);

        Assert(t.read(doc));
        AssertEq(doc.sequence(), 3);
        Assert(doc.meta() == doc_alias.meta());
        Assert(doc.body() == doc_alias.body());

        // Doc shouldn't exist outside transaction yet:
        AssertEq(aliased_db.get(nsstring_slice(@"doc")).sequence(), 0);
    }

    AssertEq(db->get(nsstring_slice(@"doc")).sequence(), 3);
    AssertEq(aliased_db.get(nsstring_slice(@"doc")).sequence(), 3);
}

- (void) createNumberedDocs {
    Transaction t(db);
    for (int i = 1; i <= 100; i++) {
        NSString* docID = [NSString stringWithFormat: @"doc-%03d", i];
        sequence seq = t.set(nsstring_slice(docID), forestdb::slice::null, nsstring_slice(docID));
        AssertEq(seq, i);
        AssertEqual((NSString*)t.get(nsstring_slice(docID)).body(),
                    docID,
                    @"(i=%d)", i);
    }
}

- (void) test04_EnumerateDocs {
    [self createNumberedDocs];
    NSLog(@"Enumerate over all docs:");
    int i = 1;
    for (DocEnumerator e(db); e; ++e, ++i) {
        NSString* expectedDocID = [NSString stringWithFormat: @"doc-%03d", i];
        AssertEqual((NSString*)e->key(), expectedDocID);
        AssertEq(e->sequence(), i);
    }
    AssertEq(i, 101);

    NSLog(@"Enumerate over range of docs:");
    i = 24;
    for (DocEnumerator e(db, nsstring_slice(@"doc-024"), nsstring_slice(@"doc-029")); e; ++e, ++i) {
        NSString* expectedDocID = [NSString stringWithFormat: @"doc-%03d", i];
        AssertEqual((NSString*)e->key(), expectedDocID);
        AssertEq(e->sequence(), i);
    }
    AssertEq(i, 30);

    NSLog(@"Enumerate over range of docs without inclusive:");
    auto opts = DocEnumerator::Options::kDefault;
    opts.inclusiveStart = opts.inclusiveEnd = false;
    i = 25;
    for (DocEnumerator e(db, nsstring_slice(@"doc-024"), nsstring_slice(@"doc-029"), opts); e; ++e, ++i) {
        NSString* expectedDocID = [NSString stringWithFormat: @"doc-%03d", i];
        AssertEqual((NSString*)e->key(), expectedDocID);
        AssertEq(e->sequence(), i);
    }
    AssertEq(i, 29);

    NSLog(@"Enumerate over vector of docs:");
    i = 0;
    std::vector<std::string> docIDs;
    docIDs.push_back("doc-005");
    docIDs.push_back("doc-029");
    docIDs.push_back("doc-023"); // out of order! (check for random-access fdb_seek)
    docIDs.push_back("doc-028");
    docIDs.push_back("doc-098");
    docIDs.push_back("doc-100");
    docIDs.push_back("doc-105");
    for (DocEnumerator e(db, docIDs); e; ++e, ++i) {
        NSLog(@"key = %@", (NSString*)e->key());
        Assert((std::string)e->key() == docIDs[i], @"Expected %s got %@",
               docIDs[i].c_str(),
               (NSString*)e->key());
    }
    AssertEq(i, 7);
}

- (void) test04_EnumerateDocsDescending {
    auto opts = DocEnumerator::Options::kDefault;
    opts.descending = true;

    [self createNumberedDocs];
    NSLog(@"Enumerate over all docs, descending:");
    int i = 100;
    for (DocEnumerator e(db, slice::null, slice::null, opts); e; ++e, --i) {
        NSString* expectedDocID = [NSString stringWithFormat: @"doc-%03d", i];
        AssertEqual((NSString*)e->key(), expectedDocID);
        AssertEq(e->sequence(), i);
    }
    AssertEq(i, 0);

    NSLog(@"Enumerate over range of docs from max, descending:");
    i = 100;
    for (DocEnumerator e(db, slice::null, nsstring_slice(@"doc-090"), opts); e; ++e, --i) {
        NSString* expectedDocID = [NSString stringWithFormat: @"doc-%03d", i];
        AssertEqual((NSString*)e->key(), expectedDocID);
        AssertEq(e->sequence(), i);
    }
    AssertEq(i, 89);

    NSLog(@"Enumerate over range of docs to min, descending:");
    i = 10;
    for (DocEnumerator e(db, nsstring_slice(@"doc-010"), slice::null, opts); e; ++e, --i) {
        NSString* expectedDocID = [NSString stringWithFormat: @"doc-%03d", i];
        AssertEqual((NSString*)e->key(), expectedDocID);
        AssertEq(e->sequence(), i);
    }
    AssertEq(i, 0);

    NSLog(@"Enumerate over range of docs, descending:");
    i = 29;
    for (DocEnumerator e(db, nsstring_slice(@"doc-029"), nsstring_slice(@"doc-024"), opts); e; ++e, --i) {
        NSString* expectedDocID = [NSString stringWithFormat: @"doc-%03d", i];
        AssertEqual((NSString*)e->key(), expectedDocID);
        AssertEq(e->sequence(), i);
    }
    AssertEq(i, 23);

    NSLog(@"Enumerate over range of docs without inclusive, descending:");
    auto optsExcl = opts;
    optsExcl.inclusiveStart = optsExcl.inclusiveEnd = false;
    i = 28;
    for (DocEnumerator e(db, nsstring_slice(@"doc-029"), nsstring_slice(@"doc-024"), optsExcl); e; ++e, --i) {
        NSString* expectedDocID = [NSString stringWithFormat: @"doc-%03d", i];
        AssertEqual((NSString*)e->key(), expectedDocID);
        AssertEq(e->sequence(), i);
    }
    AssertEq(i, 24);

    NSLog(@"Enumerate over vector of docs, descending:");
    std::vector<std::string> docIDs;
    docIDs.push_back("doc-005");
    docIDs.push_back("doc-029");
    docIDs.push_back("doc-023"); // out of order! (check for random-access fdb_seek)
    docIDs.push_back("doc-028");
    docIDs.push_back("doc-098");
    docIDs.push_back("doc-100");
    docIDs.push_back("doc-105");
    i = (int)docIDs.size() - 1;
    for (DocEnumerator e(db, docIDs, opts); e; ++e, --i) {
        NSLog(@"key = %@", (NSString*)e->key());
        Assert((std::string)e->key() == docIDs[i], @"Expected %s got %@",
               docIDs[i].c_str(),
               (NSString*)e->key());
    }
    AssertEq(i, -1);
}

- (void) test05_AbortTransaction {
    // Initial document:
    Transaction(db).set(nsstring_slice(@"a"), nsstring_slice(@"A"));
    {
        Transaction t(db);
        t.set(nsstring_slice(@"x"), nsstring_slice(@"X"));
        t.set(nsstring_slice(@"a"), nsstring_slice(@"Z"));
        AssertEqual((NSString*)t.get(nsstring_slice(@"a")).body(), @"Z");
        AssertEqual((NSString*)db->get(nsstring_slice(@"a")).body(), @"Z");
        t.abort();
    }
    AssertEqual((NSString*)db->get(nsstring_slice(@"a")).body(), @"A");
    AssertEq(db->get(nsstring_slice(@"x")).sequence(), 0);
}


// Test for MB-12287
- (void) test06_TransactionsThenIterate {
    Database db2(kDBPath, FDB_OPEN_FLAG_CREATE, Database::defaultConfig());

    const NSUInteger kNTransactions = 42; // 41 is ok, 42+ fails
    const NSUInteger kNDocs = 100;

    for (NSUInteger t = 1; t <= kNTransactions; t++) {
        Transaction trans(db);
        for (NSUInteger d = 1; d <= kNDocs; d++) {
            NSString* docID = [NSString stringWithFormat: @"%03lu.%03lu", t, d];
            trans.set(nsstring_slice(docID), nsstring_slice(@"some document content goes here"));
        }
    }

    int i = 0;
    for (DocEnumerator iter(&db2); iter; ++iter) {
        NSString* key = (NSString*)(*iter).key();
        //NSLog(@"key = %@", key);
        NSUInteger t = (i / kNDocs) + 1;
        NSUInteger d = (i % kNDocs) + 1;
        XCTAssertEqualObjects(key, ([NSString stringWithFormat: @"%03lu.%03lu", t, d]));
        i++;
    }
}

@end
