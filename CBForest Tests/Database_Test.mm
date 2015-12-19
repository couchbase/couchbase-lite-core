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

using namespace cbforest;


#define ENCRYPT_DATABASES 1


@interface Database_Test : XCTestCase
@end

@implementation Database_Test
{
    std::string dbPath;
    Database* db;
}

static NSString* sTestDir;

void CreateTestDir() {
    if (!sTestDir)
        sTestDir = [NSTemporaryDirectory() stringByAppendingPathComponent: @"CBForest_Unit_Tests"];
    NSError* error;
    [[NSFileManager defaultManager] removeItemAtPath: sTestDir error: nil];
    BOOL ok = [[NSFileManager defaultManager] createDirectoryAtPath: sTestDir
                                        withIntermediateDirectories: NO
                                                         attributes: nil
                                                              error: &error];
    assert(ok);
}

std::string PathForDatabaseNamed(NSString *name) {
    const char *path = [sTestDir stringByAppendingPathComponent: name].fileSystemRepresentation;
    ::unlink(path);
    return std::string(path);
}

static void randomAESKey(fdb_encryption_key &key) {
    key.algorithm = FDB_ENCRYPTION_AES256;
    SecRandomCopyBytes(kSecRandomDefault, sizeof(key.bytes), (uint8_t*)&key.bytes);
}

Database::config TestDBConfig() {
    auto config = Database::defaultConfig();
#if ENCRYPT_DATABASES
    randomAESKey(config.encryption_key);
#endif
    return config;
}

+ (void) initialize {
    if (self == [Database_Test class]) {
        LogLevel = kWarning;
    }
}

- (void)setUp
{
    [super setUp];
    CreateTestDir();
    dbPath = PathForDatabaseNamed(@"forest_temp.fdb");
    db = new Database(dbPath, TestDBConfig());
}

- (void)tearDown
{
    if (db) {
        db->deleteDatabase();
        delete db;
    }
    [super tearDown];
}

- (void) invokeTest {
    try{
        [super invokeTest];
    } catch (error e) {
        XCTFail(@"Exception: %s (%d)", e.message(), e.status);
    }
}

- (void) test01_DbInfo {
    auto info = db->getInfo();
    AssertEq(info.doc_count, 0u);
    AssertEq(info.space_used, 0u);
    Assert(info.file_size > 0);

    AssertEq(db->lastSequence(), 0u);
}

- (void)test02_CreateDoc
{
    alloc_slice key("key");
    {
        Transaction t(db);
        t.set(key, nsstring_slice(@"value"));
    }
    AssertEq(db->lastSequence(), 1u);
    Document doc = db->get(key);
    Assert(doc.key() == key);
    Assert(doc.body() == nsstring_slice(@"value"));
}

- (void) test03_SaveDocs {
    Transaction(db).set(nsstring_slice(@"a"), nsstring_slice(@"A"));   //WORKAROUND: Add a doc before the main transaction so it doesn't start at sequence 0

    Database aliased_db(dbPath, TestDBConfig());
    AssertEqual((NSString*)aliased_db.get(nsstring_slice(@"a")).body(), @"A");

    {
        Transaction t(db);
        Document doc(nsstring_slice(@"doc"));
        doc.setMeta(nsstring_slice(@"m-e-t-a"));
        doc.setBody(nsstring_slice(@"THIS IS THE BODY"));
        t.write(doc);

        AssertEq(doc.sequence(), 2u);
        AssertEq(db->lastSequence(), 2u);
        auto doc_alias = t.get(doc.sequence());
        Assert(doc_alias.key() == doc.key());
        Assert(doc_alias.meta() == doc.meta());
        Assert(doc_alias.body() == doc.body());

        doc_alias.setBody(nsstring_slice(@"NU BODY"));
        t.write(doc_alias);

        Assert(t.read(doc));
        AssertEq(doc.sequence(), 3u);
        Assert(doc.meta() == doc_alias.meta());
        Assert(doc.body() == doc_alias.body());

        // Doc shouldn't exist outside transaction yet:
        AssertEq(aliased_db.get(nsstring_slice(@"doc")).sequence(), 0u);
    }

    AssertEq(db->get(nsstring_slice(@"doc")).sequence(), 3u);
    AssertEq(aliased_db.get(nsstring_slice(@"doc")).sequence(), 3u);
}

- (void) createNumberedDocs {
    Transaction t(db);
    for (int i = 1; i <= 100; i++) {
        NSString* docID = [NSString stringWithFormat: @"doc-%03d", i];
        sequence seq = t.set(nsstring_slice(docID), cbforest::slice::null, nsstring_slice(docID));
        AssertEq(seq, (sequence)i);
        AssertEqual((NSString*)t.get(nsstring_slice(docID)).body(),
                    docID,
                    @"(i=%d)", i);
    }
}

- (void) test04_EnumerateDocs {
    {
        NSLog(@"Enumerate empty db");
        int i = 0;
        DocEnumerator e(*db);
        for (; e.next(); ++i) {
            XCTFail(@"Shouldn't have found any docs");
        }
        Assert(!e);
    }

    [self createNumberedDocs];

    for (int metaOnly=0; metaOnly <= 1; ++metaOnly) {
        NSLog(@"Enumerate over all docs (metaOnly=%d)", metaOnly);
        auto opts = DocEnumerator::Options::kDefault;
        opts.contentOptions = metaOnly ? KeyStore::kMetaOnly : KeyStore::kDefaultContent;

        {
            int i = 1;
            DocEnumerator e(*db, slice::null, slice::null, opts);
            for (; e.next(); ++i) {
                NSString* expectedDocID = [NSString stringWithFormat: @"doc-%03d", i];
                AssertEqual((NSString*)e->key(), expectedDocID);
                AssertEq(e->sequence(), (sequence)i);
                Assert(e->body().size > 0); // even metaOnly should set the body length
                Assert(e->offset() > 0);
            }
            AssertEq(i, 101);
            Assert(!e);
        }

        NSLog(@"Enumerate over range of docs:");
        int i = 24;
        for (DocEnumerator e(*db, nsstring_slice(@"doc-024"), nsstring_slice(@"doc-029"), opts); e.next(); ++i) {
            NSString* expectedDocID = [NSString stringWithFormat: @"doc-%03d", i];
            AssertEqual((NSString*)e->key(), expectedDocID);
            AssertEq(e->sequence(), (sequence)i);
            Assert(e->body().size > 0); // even metaOnly should set the body length
            Assert(e->offset() > 0);
        }
        AssertEq(i, 30);

        NSLog(@"Enumerate over range of docs without inclusive:");
        opts.inclusiveStart = opts.inclusiveEnd = false;
        i = 25;
        for (DocEnumerator e(*db, nsstring_slice(@"doc-024"), nsstring_slice(@"doc-029"), opts); e.next(); ++i) {
            NSString* expectedDocID = [NSString stringWithFormat: @"doc-%03d", i];
            AssertEqual((NSString*)e->key(), expectedDocID);
            AssertEq(e->sequence(), (sequence)i);
            Assert(e->body().size > 0); // even metaOnly should set the body length
            Assert(e->offset() > 0);
        }
        AssertEq(i, 29);
        opts.inclusiveStart = opts.inclusiveEnd = true;

        NSLog(@"Enumerate over vector of docs:");
        i = 0;
        std::vector<std::string> docIDs;
        docIDs.push_back("doc-005");
        docIDs.push_back("doc-029");
        docIDs.push_back("doc-023"); // out of order! (check for random-access fdb_seek)
        docIDs.push_back("doc-028");
        docIDs.push_back("doc-098");
        docIDs.push_back("doc-100");
        docIDs.push_back("doc-105"); // doesn't exist!
        for (DocEnumerator e(*db, docIDs, opts); e.next(); ++i) {
            NSLog(@"key = %@", (NSString*)e->key());
            Assert((std::string)e->key() == docIDs[i], @"Expected %s got %@",
                   docIDs[i].c_str(),
                   (NSString*)e->key());
            AssertEq(e->exists(), i < 6);
            if (i < 6) {
                Assert(e->body().size > 0); // even metaOnly should set the body length
                Assert(e->offset() > 0);
            }
        }
        AssertEq(i, 7);
    }
}

- (void) test04_EnumerateDocsDescending {
    auto opts = DocEnumerator::Options::kDefault;
    opts.descending = true;

    [self createNumberedDocs];
    NSLog(@"Enumerate over all docs, descending:");
    int i = 100;
    for (DocEnumerator e(*db, slice::null, slice::null, opts); e.next(); --i) {
        NSString* expectedDocID = [NSString stringWithFormat: @"doc-%03d", i];
        AssertEqual((NSString*)e->key(), expectedDocID);
        AssertEq(e->sequence(), (sequence)i);
    }
    AssertEq(i, 0);

    NSLog(@"Enumerate over range of docs from max, descending:");
    i = 100;
    for (DocEnumerator e(*db, slice::null, nsstring_slice(@"doc-090"), opts); e.next(); --i) {
        NSString* expectedDocID = [NSString stringWithFormat: @"doc-%03d", i];
        AssertEqual((NSString*)e->key(), expectedDocID);
        AssertEq(e->sequence(), (sequence)i);
    }
    AssertEq(i, 89);

    NSLog(@"Enumerate over range of docs to min, descending:");
    i = 10;
    for (DocEnumerator e(*db, nsstring_slice(@"doc-010"), slice::null, opts); e.next(); --i) {
        NSString* expectedDocID = [NSString stringWithFormat: @"doc-%03d", i];
        AssertEqual((NSString*)e->key(), expectedDocID);
        AssertEq(e->sequence(), (sequence)i);
    }
    AssertEq(i, 0);

    NSLog(@"Enumerate over range of docs, descending:");
    i = 29;
    for (DocEnumerator e(*db, nsstring_slice(@"doc-029"), nsstring_slice(@"doc-024"), opts); e.next(); --i) {
        NSString* expectedDocID = [NSString stringWithFormat: @"doc-%03d", i];
        AssertEqual((NSString*)e->key(), expectedDocID);
        AssertEq(e->sequence(), (sequence)i);
    }
    AssertEq(i, 23);

    NSLog(@"Enumerate over range of docs, descending, max key doesn't exist:");
    i = 29;
    for (DocEnumerator e(*db, nsstring_slice(@"doc-029b"), nsstring_slice(@"doc-024"), opts); e.next(); --i) {
        NSString* expectedDocID = [NSString stringWithFormat: @"doc-%03d", i];
        AssertEqual((NSString*)e->key(), expectedDocID);
        AssertEq(e->sequence(), (sequence)i);
    }
    AssertEq(i, 23);

    NSLog(@"Enumerate over range of docs without inclusive, descending:");
    auto optsExcl = opts;
    optsExcl.inclusiveStart = optsExcl.inclusiveEnd = false;
    i = 28;
    for (DocEnumerator e(*db, nsstring_slice(@"doc-029"), nsstring_slice(@"doc-024"), optsExcl); e.next(); --i) {
        NSString* expectedDocID = [NSString stringWithFormat: @"doc-%03d", i];
        AssertEqual((NSString*)e->key(), expectedDocID);
        AssertEq(e->sequence(), (sequence)i);
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
    for (DocEnumerator e(*db, docIDs, opts); e.next(); --i) {
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
    AssertEq(db->get(nsstring_slice(@"x")).sequence(), 0u);
}


// Test for MB-12287
- (void) test06_TransactionsThenIterate {
    Database db2(dbPath, TestDBConfig());

    const NSUInteger kNTransactions = 42; // 41 is ok, 42+ fails
    const NSUInteger kNDocs = 100;

    for (NSUInteger t = 1; t <= kNTransactions; t++) {
        Transaction trans(db);
        for (NSUInteger d = 1; d <= kNDocs; d++) {
            NSString* docID = [NSString stringWithFormat: @"%03lu.%03lu", (unsigned long)t, (unsigned long)d];
            trans.set(nsstring_slice(docID), nsstring_slice(@"some document content goes here"));
        }
    }

    int i = 0;
    for (DocEnumerator iter(db2); iter.next(); ) {
        NSString* key = (NSString*)(*iter).key();
        //NSLog(@"key = %@", key);
        NSUInteger t = (i / kNDocs) + 1;
        NSUInteger d = (i % kNDocs) + 1;
        XCTAssertEqualObjects(key, ([NSString stringWithFormat: @"%03lu.%03lu", (unsigned long)t, (unsigned long)d]));
        i++;
    }
}

- (void) test07_DeleteKey {
    slice key("a");
    Transaction(db).set(key, nsstring_slice(@"A"));
    Transaction(db).del(key);
    Document doc = db->get(key);
//    Assert(doc.deleted());
    Assert(!doc.exists());
}

- (void) test07_DeleteDoc {
    slice key("a");
    Transaction(db).set(key, nsstring_slice(@"A"));

    {
        Transaction t(db);
        Document doc = db->get(key);
        t.del(doc);
    }

    Document doc = db->get(key);
//    Assert(doc.deleted());
    Assert(!doc.exists());
}

- (void) test08_KeyStoreInfo {
    KeyStore s(db, "store");
    AssertEq(s.lastSequence(), 0u);
    Assert(s.name() == "store");

    auto info = s.getInfo();
    AssertEq(info.doc_count, 0u);
    AssertEq(info.space_used, 0u);
    AssertEq(info.last_seqnum, 0u);
    AssertEq(strcmp(info.name, "store"), 0);
}

- (void) test09_KeyStoreWrite {
    KeyStore s(db, "store");
    alloc_slice key("key");
    {
        Transaction t(db);
        t(s).set(key, nsstring_slice(@"value"));
    }
    AssertEq(s.lastSequence(), 1u);
    Document doc = s.get(key);
    Assert(doc.key() == key);
    Assert(doc.body() == nsstring_slice(@"value"));

    Document doc2 = db->get(key);
    Assert(!doc2.exists());
}

- (void) test10_KeyStoreDelete {
    KeyStore s(db, "store");
    alloc_slice key("key");
//    {
//        Transaction t(db);
//        t(s).set(key, nsstring_slice(@"value"));
//    }
    {
        Transaction t(db);
        s.erase(t);
    }
    AssertEq(s.lastSequence(), 0u);
    Document doc = s.get(key);
    Assert(!doc.exists());
}

- (void) test11_ReadOnly {
    {
        Transaction t(db);
        t.set(slice("key"), nsstring_slice(@"value"));
    }
    // Reopen db as read-only:
    Database::config config = db->getConfig();
    NSLog(@"//// Closing db");
    delete db;
    db = nil;
    NSLog(@"//// Reopening db");
    config.flags = FDB_OPEN_FLAG_RDONLY;
    db = new Database(dbPath, config);

    auto doc = db->get(slice("key"));
    Assert(doc.exists());

    // Attempt to change a doc:
    int status = 0;
    try {
        Transaction t(db);
        // This is expected to throw an exception:
        t.set(slice("key"), nsstring_slice(@"somethingelse"));
    } catch (error x) {
        status = x.status;
    }
    AssertEq(status, FDB_RESULT_RONLY_VIOLATION);

    // Now try to open a nonexistent db, without the CREATE flag:
    status = 0;
    try {
        Database db2("/tmp/db_non_existent", config);
    } catch (error x) {
        status = x.status;
    }
    AssertEq(status, FDB_RESULT_NO_SUCH_FILE);
}

#if 0
- (void) test12_Copy {
    [self createNumberedDocs];

    std::string newPath = PathForDatabaseNamed(@"encryptedCopy");
    Database::encryptionConfig enc;
#if ENCRYPT_DATABASES
    enc.encrypted = true;
    SecRandomCopyBytes(kSecRandomDefault, sizeof(enc.encryptionKey),
                       (uint8_t*)&enc.encryptionKey);
#else
    enc.encrypted = false;
#endif

    std::string filename = db->filename();
    db->copyToFile(newPath, enc);
    AssertEq(db->filename(), filename);

    Database::config config = Database::defaultConfig();
    *(Database::encryptionConfig*)&config = enc;
    Database newDB(newPath, config);

    Document doc = newDB.get(slice("doc-001"));
    Assert(doc.exists());
}
#endif


static Database_Test *sCurrentTest;
static Database* sExpectedCompactingDB;
static int sNumCompactCalls;

static void onCompact(Database* db, bool compacting) {
    id self = sCurrentTest;
    AssertEq(db, sExpectedCompactingDB);
    Assert(sNumCompactCalls < 2);
    if (sNumCompactCalls == 0)
        Assert(compacting);
    else
        Assert(!compacting);
    ++sNumCompactCalls;
}

- (void) test13_Compact {
    [self createNumberedDocs];

    {
        Transaction t(db);
        for (int i = 1; i <= 100; i += 3) {
            NSString* docID = [NSString stringWithFormat: @"doc-%03d", i];
            Document doc = db->get((nsstring_slice)docID);
            t.del(doc);
        }
    }

    Database::onCompactCallback = onCompact;
    sCurrentTest = self;
    sExpectedCompactingDB = db;
    sNumCompactCalls = 0;

    db->compact();

    Database::onCompactCallback = NULL;
    AssertEq(sNumCompactCalls, 2);
}


- (void) test14_rekey {
    Database::config config = db->getConfig();
    [self createNumberedDocs];
    fdb_encryption_key newKey;
    randomAESKey(newKey);

    db->rekey(newKey);
    delete db;
    db = nil;

    config.encryption_key = newKey;
    db = new Database(dbPath, config);
    Document doc = db->get((slice)"doc-001");
    Assert(doc.exists());
}

@end
