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

using namespace forestdb;

#define kDBPath "/tmp/forest.db"


@interface Database_Test : XCTestCase
@end

@implementation Database_Test
{
    Database* db;
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
        t.set(key, @"value");
    }
    Document doc = db->get(key);
    Assert(doc.key().equal(key));
    Assert(doc.body().equal(@"value"));
}

- (void) test03_SaveDocs {
    Transaction(db).set(@"a", @"A");   //WORKAROUND: Add a doc before the main transaction so it doesn't start at sequence 0

    Database aliased_db(kDBPath, 0, Database::defaultConfig());
    AssertEqual((NSString*)aliased_db.get(@"a").body(), @"A");

    {
        Transaction t(db);
        Document doc(@"doc");
        doc.setMeta(@"m-e-t-a");
        doc.setBody(@"THIS IS THE BODY");
        t.write(doc);

        AssertEq(doc.sequence(), 2);
        auto doc_alias = t.get(doc.sequence());
        Assert(doc_alias.key().equal(doc.key()));
        Assert(doc_alias.meta().equal(doc.meta()));
        Assert(doc_alias.body().equal(doc.body()));

        doc_alias.setBody(@"NU BODY");
        t.write(doc_alias);

        Assert(t.read(doc));
        AssertEq(doc.sequence(), 3);
        Assert(doc.meta().equal(doc_alias.meta()));
        Assert(doc.body().equal(doc_alias.body()));

        // Doc shouldn't exist outside transaction yet:
        AssertEq(aliased_db.get(@"doc").sequence(), 0);
    }

    AssertEq(db->get(@"doc").sequence(), 3);
    AssertEq(aliased_db.get(@"doc").sequence(), 3);
}

- (void) createNumberedDocs {
    Transaction t(db);
    for (int i = 1; i <= 100; i++) {
        NSString* docID = [NSString stringWithFormat: @"doc-%03d", i];
        sequence seq = t.set(docID, forestdb::slice::null, docID);
        AssertEq(seq, i);
        AssertEqual((NSString*)t.get(docID).body(),
                    docID,
                    @"(i=%d)", i);
    }
}

- (void) test04_EnumerateDocs {
    [self createNumberedDocs];
    int i = 1;
    for (auto e = db->enumerate(); e; ++e, ++i) {
        NSString* expectedDocID = [NSString stringWithFormat: @"doc-%03d", i];
        AssertEqual((NSString*)e->key(), expectedDocID);
        AssertEq(e->sequence(), i);
    }
    AssertEq(i, 101);

    NSLog(@"Enumerate over vector of docs:");
    i = 0;
    std::vector<std::string> docIDs;
    docIDs.push_back("doc-005");
    docIDs.push_back("doc-023");
    docIDs.push_back("doc-028");
    docIDs.push_back("doc-029");
    docIDs.push_back("doc-098");
    docIDs.push_back("doc-100");
    docIDs.push_back("doc-105");
    for (auto e = db->enumerate(docIDs); e; ++e, ++i) {
        NSLog(@"key = %@", (NSString*)e->key());
        Assert((std::string)e->key() == docIDs[i], @"Expected %s got %@",
               docIDs[i].c_str(),
               (NSString*)e->key());
    }
    AssertEq(i, 7);
}

- (void) test05_AbortTransaction {
    // Initial document:
    Transaction(db).set(@"a", @"A");
    {
        Transaction t(db);
        t.set(@"x", @"X");
        t.set(@"a", @"Z");
        AssertEqual((NSString*)t.get(@"a").body(), @"Z");
        AssertEqual((NSString*)db->get(@"a").body(), @"Z");
        t.abort();
    }
    AssertEqual((NSString*)db->get(@"a").body(), @"A");
    AssertEq(db->get(@"x").sequence(), 0);
}

@end
