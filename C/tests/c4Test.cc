//
// c4Test.cc
//
// Copyright 2015-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "c4Test.hh"       // IWYU pragma: keep
#include "TestsCommon.hh"  // IWYU pragma: keep
#include "c4BlobStore.h"
#include "c4Collection.h"
#include "c4Document+Fleece.h"
#include "fleece/slice.hh"
#include "FilePath.hh"
#include "StringUtil.hh"  // IWYU pragma: keep
#include "Backtrace.hh"
#include "Stopwatch.hh"
#include "Error.hh"
#include <csignal>
#include <iostream>
#include <fstream>
#include <cstdio>
#include <thread>
#include <mutex>
#include <chrono>
#include <utility>

#if TARGET_OS_IPHONE
#    include <CoreFoundation/CFBundle.h>
#endif

#ifdef _MSC_VER
#    include <atlbase.h>
#endif

using namespace std;

const std::string& TempDir() {
    static string kTempDir = GetTempDirectory().path();
    return kTempDir;
}

// Debugging utility to print a slice -- in LLDB enter "call ps(___)"
void ps(C4Slice s);

void ps(C4Slice s) { cerr << s << "\n"; }

void ps(fleece::slice s);

void ps(fleece::slice s) { ps(C4Slice{s.buf, s.size}); }

ostream& operator<<(ostream& out, C4Error error) {
    if ( error.code ) {
        C4SliceResult s = c4error_getDescription(error);
        // Ensure that s.size (size_t/uint64) will not exceed limit of std::streamsize (int64)
        DebugAssert(s.size <= std::numeric_limits<std::streamsize>::max());
        out << "C4Error(";
        out.write((const char*)s.buf, s.size);  // NOLINT(cppcoreguidelines-narrowing-conversions)
        out << ")";
        c4slice_free(s);

        alloc_slice backtrace = c4error_getBacktrace(error);
        if ( backtrace ) {
            // Ensure that backtrace.size (size_t/uint64) will not exceed limit of std::streamsize (int64)
            DebugAssert(backtrace.size <= std::numeric_limits<std::streamsize>::max());
            out << ":\n";
            out.write((const char*)backtrace.buf, backtrace.size);  // NOLINT(cppcoreguidelines-narrowing-conversions)
        }
    } else {
        out << "C4Error(none)";
    }
    return out;
}

ERROR_INFO::~ERROR_INFO() {
    if ( _error->code ) {
        if ( OnMainThread() ) UNSCOPED_INFO(*_error);
        else
            std::cerr << "ERROR_INFO: " << *_error << '\n';
    }
}

WITH_ERROR::~WITH_ERROR() {
    if ( _error->code ) {
        // Unfortunately it's too late to use UNSCOPED_INFO; since WITH_ERROR is used inside the
        // CHECK/REQUIRE macro, by the time it's destructed Catch has already registered the error.
        // But we can tell Catch to warn about it, which will show up below its failure message.
        if ( OnMainThread() ) WARN(*_error);
        else
            std::cerr << "WITH_ERROR: " << *_error << '\n';
    }
}

void CheckError(C4Error error, C4ErrorDomain expectedDomain, int expectedCode, const char* expectedMessage) {
    Check(error == (C4Error{expectedDomain, expectedCode}));
    if ( expectedMessage ) {
        alloc_slice msg = c4error_getMessage(error);
        Check(msg == expectedMessage);
    }
}

void C4ExpectException(C4ErrorDomain domain, int code, const std::function<void()>& lambda) {
    try {
        ExpectingExceptions x;
        C4Log("NOTE: Expecting an exception to be thrown...");
        lambda();
    } catch ( const std::exception& x ) {
        auto    e   = litecore::error::convertException(x).standardized();
        C4Error err = {C4ErrorDomain(e.domain), e.code};
        char    buffer[256];
        C4Log("... caught exception %s", c4error_getDescriptionC(err, buffer, sizeof(buffer)));
        Check(err == (C4Error{domain, code}));
        return;
    }
    FAIL("Should have thrown an exception");
}

#pragma mark - C4TEST CLASS

#if defined(CMAKE) && defined(_MSC_VER)
string C4Test::sFixturesDir           = "../C/tests/data/";
string C4Test::sReplicatorFixturesDir = "../Replicator/tests/data/";
#else
string C4Test::sFixturesDir           = "C/tests/data/";
string C4Test::sReplicatorFixturesDir = "Replicator/tests/data/";
#endif


C4Test::C4Test(int num) : _storage(kC4SQLiteStorageEngine) {  // NOLINT(cppcoreguidelines-pro-type-member-init)
    constexpr static TestOptions numToTestOption[] = {
#if SkipVersionVectorTest
        RevTreeOption,
        EncryptedRevTreeOption
#else
        RevTreeOption,
        VersionVectorOption,
        EncryptedRevTreeOption
#endif
    };
    static_assert(sizeof(numToTestOption) / sizeof(TestOptions) >= numberOfOptions);
    TestOptions testOption = numToTestOption[num];

    static once_flag once;
    call_once(once, [] {
        InitTestLogging();

        auto enc = FLEncoder_New();
        FLEncoder_BeginDict(enc, 1);
        FLEncoder_WriteKey(enc, FLSTR("ans*wer"));
        FLEncoder_WriteInt(enc, 42);
        FLEncoder_EndDict(enc);
        auto result = FLEncoder_Finish(enc, nullptr);
        kFleeceBody = {result.buf, result.size};
        FLEncoder_Free(enc);

        enc = FLEncoder_New();
        FLEncoder_BeginDict(enc, 1);
        FLEncoder_EndDict(enc);
        result           = FLEncoder_Finish(enc, nullptr);
        kEmptyFleeceBody = {result.buf, result.size};
        FLEncoder_Free(enc);

#if TARGET_OS_IPHONE
        static once_flag once;
        call_once(once, [] {
            // iOS tests copy the fixture files into the test bundle.
            CFBundleRef bundle = CFBundleGetBundleWithIdentifier(CFSTR("org.couchbase.LiteCoreTests"));
            Require(bundle);
            CFURLRef    url  = CFBundleCopyResourcesDirectoryURL(bundle);
            CFStringRef path = CFURLCopyFileSystemPath(url, kCFURLPOSIXPathStyle);
            char        resourcesDir[1024];
            Require(CFStringGetCString(path, resourcesDir, sizeof(resourcesDir), kCFStringEncodingUTF8));
            sFixturesDir           = string(resourcesDir) + "/TestData/C/tests/data/";
            sReplicatorFixturesDir = string(resourcesDir) + "/TestData/Replicator/tests/data/";
            C4Log("Fixtures dir: %s", sFixturesDir.c_str());
            CFRelease(path);
            CFRelease(url);
        });
#endif
    });

    Check(c4_shutdown(nullptr));

    objectCount = c4_getObjectCount();

    _dbConfig = {slice(TempDir()), kC4DB_Create};
    if ( testOption == VersionVectorOption ) {
        _dbConfig.flags |= kC4DB_VersionVectors | kC4DB_FakeVectorClock;
        kRev1ID = kRevID = kRev1ID_Alt = C4STR("1@*");
        kRev2ID                        = C4STR("2@*");
        kRev3ID                        = C4STR("3@*");
        kRev4ID                        = C4STR("4@*");
    } else {
        kRev1ID = kRevID = C4STR("1-abcd");
        kRev1ID_Alt      = C4STR("1-dcba");
        kRev2ID          = C4STR("2-c001d00d");
        kRev3ID          = C4STR("3-deadbeef");
        kRev4ID          = C4STR("4-44444444");
    }

    if ( testOption == EncryptedRevTreeOption ) {
        _dbConfig.encryptionKey.algorithm = kC4EncryptionAES256;
        memcpy(_dbConfig.encryptionKey.bytes, "this is not a random key at all.", kC4EncryptionKeySizeAES256);
    }

    C4Error error;
    if ( !c4db_deleteNamed(kDatabaseName, _dbConfig.parentDirectory, ERROR_INFO(&error)) ) Require(error.code == 0);
    db = c4db_openNamed(kDatabaseName, &_dbConfig, ERROR_INFO(&error));
    Require(db != nullptr);
}

C4Test::~C4Test() {
    if ( db ) {
        Check(c4db_delete(db, WITH_ERROR()));
        c4db_release(db);
    }

    if ( !current_exception() ) {
        // Check for leaks:
        if ( !WaitUntil(20s, [&] { return c4_getObjectCount() - objectCount == 0; }) ) {
            FAIL_CHECK("LiteCore objects were leaked by this test:");
            fprintf(stderr, "*** LEAKED LITECORE OBJECTS: \n");
            c4_dumpInstances();
            fprintf(stderr, "***\n");
        }
    }
}

C4Database* C4Test::createDatabase(const string& nameSuffix) {
    Require(!nameSuffix.empty());
    string  name = string(kDatabaseName) + "_" + nameSuffix;
    C4Error error;
    if ( !c4db_deleteNamed(slice(name), _dbConfig.parentDirectory, ERROR_INFO(&error)) ) Require(error.code == 0);
    auto newDB = c4db_openNamed(slice(name), &_dbConfig, ERROR_INFO());
    Require(newDB != nullptr);
    return newDB;
}

C4Collection* C4Test::requireCollection(C4Database* db, C4CollectionSpec spec) {
    C4Collection* coll = c4db_getCollection(db, spec, ERROR_INFO());
    Require(coll);
    return coll;
}

void C4Test::closeDB() {
    Require(c4db_close(db, WITH_ERROR()));
    c4db_release(db);
    db = nullptr;
}

void C4Test::syncDBConfig() {
    Require(db);
    _dbConfig.flags         = c4db_getConfig2(db)->flags;
    _dbConfig.encryptionKey = c4db_getConfig2(db)->encryptionKey;
}

void C4Test::reopenDB() {
    // Update _dbConfig in case db was reopened with different flags or encryption:
    syncDBConfig();

    closeDB();
    db = c4db_openNamed(kDatabaseName, &_dbConfig, ERROR_INFO());
    Require(db);
}

void C4Test::reopenDBReadOnly() { reopenDBNewFlags(~kC4DB_Create, kC4DB_ReadOnly); }

void C4Test::reopenDBNewFlags(C4DatabaseFlags andFlags, C4DatabaseFlags orFlags) {
    Require(c4db_close(db, WITH_ERROR()));
    c4db_release(db);
    db              = nullptr;
    _dbConfig.flags = (_dbConfig.flags & andFlags) | orFlags;
    db              = c4db_openNamed(kDatabaseName, &_dbConfig, ERROR_INFO());
    Require(db);
}

void C4Test::deleteDatabase() {
    bool deletedDb = c4db_delete(db, ERROR_INFO());
    Require(deletedDb);
    c4db_release(db);
    db = nullptr;
}

void C4Test::deleteAndRecreateDB(C4Database*& db) {
    // Have to copy the name and parentDir -- their slices are invalidated when db is released.
    alloc_slice       name(c4db_getName(db));
    C4DatabaseConfig2 config = *c4db_getConfig2(db);
    alloc_slice       parentDir(config.parentDirectory);
    config.parentDirectory = parentDir;

    Require(c4db_delete(db, WITH_ERROR()));
    c4db_release(db);
    db = nullptr;

    db = c4db_openNamed(name, &config, ERROR_INFO());
    Require(db);
}

/*static*/ alloc_slice C4Test::copyFixtureDB(const string& name) { return copyFixtureDB(sFixturesDir, name); }

alloc_slice C4Test::copyFixtureDB(const string& parentDir, const string& name) {
    auto               srcPath = litecore::FilePath(parentDir + name, "");
    litecore::FilePath destDir(TempDir(), "");
    auto               dbPath = destDir[srcPath.fileOrDirName() + "/"];
    dbPath.delRecursive();
    srcPath.copyTo(dbPath);
    return alloc_slice(dbPath.unextendedName());
}

/*static*/ C4Collection* C4Test::createCollection(C4Database* db, C4CollectionSpec spec) {
    auto coll = c4db_createCollection(db, spec, ERROR_INFO());
    Require(coll);
    return coll;
}

/*static*/ C4Collection* C4Test::getCollection(C4Database* db, C4CollectionSpec spec, bool mustExist) {
    auto coll = c4db_getCollection(db, spec, ERROR_INFO());
    if ( mustExist ) { Require(coll); }
    return coll;
}

int C4Test::addDocs(C4Collection* collection, int total, const std::string& idprefix, bool newRev) const {
    int              docNo   = 1;
    constexpr size_t bufSize = 80;
    for ( int i = 1; docNo <= total; i++ ) {
        C4Log("-------- Creating %d docs --------", i);
        TransactionHelper t(c4coll_getDatabase(collection));
        char              docID[bufSize];
        snprintf(docID, bufSize, "%s%d", idprefix.c_str(), docNo++);
        if ( newRev ) createNewRev(collection, c4str(docID), kFleeceBody);
        else
            createRev(collection, c4str(docID), (isRevTrees() ? "1-11"_sl : "1@*"_sl), kFleeceBody);
    }
    C4Log("-------- Done creating docs --------");
    return docNo - 1;
}

int C4Test::addDocs(C4Database* database, C4CollectionSpec spec, int total, std::string idprefix) const {
    C4Collection* coll = getCollection(database, spec);
    if ( idprefix.empty() ) { idprefix = (database == db ? "newdoc-db-" : "newdoc-otherdb-"); }
    return addDocs(coll, total, idprefix);
}

void C4Test::createRev(C4Slice docID, C4Slice revID, C4Slice body, C4RevisionFlags flags) const {
    C4Test::createRev(db, docID, revID, body, flags);
}

void C4Test::createRev(C4Database* db, C4Slice docID, C4Slice revID, C4Slice body, C4RevisionFlags flags) {
    createRev(c4db_getDefaultCollection(db, nullptr), docID, revID, body, flags);
}

void C4Test::createRev(C4Collection* collection, C4Slice docID, C4Slice revID, C4Slice body, C4RevisionFlags flags) {
    C4Database*       db = c4coll_getDatabase(collection);
    TransactionHelper t(db);
    C4Error           error;
    auto              curDoc = c4coll_getDoc(collection, docID, false, kDocGetAll, &error);

    Require(curDoc != nullptr);
    alloc_slice parentID;
    if ( isRevTrees(db) ) parentID = curDoc->revID;
    else
        parentID = c4doc_getRevisionHistory(curDoc, 0, nullptr, 0);
    createConflictingRev(collection, docID, parentID, revID, body, flags);
    c4doc_release(curDoc);
}

void C4Test::createConflictingRev(C4Database* db, C4Slice docID, C4Slice parentRevID, C4Slice newRevID, C4Slice body,
                                  C4RevisionFlags flags) {
    createConflictingRev(requireCollection(db), docID, parentRevID, newRevID, body, flags);
}

void C4Test::createConflictingRev(C4Collection* collection, C4Slice docID, C4Slice parentRevID, C4Slice newRevID,
                                  C4Slice body, C4RevisionFlags flags) {
    C4Slice         history[2] = {newRevID, parentRevID};
    C4DocPutRequest rq         = {};
    rq.existingRevision        = true;
    rq.allowConflict           = true;
    rq.docID                   = docID;
    rq.history                 = history;
    rq.historyCount            = 1 + (parentRevID.buf != nullptr);
    rq.body                    = body;
    rq.revFlags                = flags;
    rq.save                    = true;
    size_t commonAncestor;
    auto   doc = c4coll_putDoc(collection, &rq, &commonAncestor, ERROR_INFO());
    Require(doc != nullptr);
    if ( commonAncestor == 0 )
        C4Warn("createConflictingRev: doc %.*s rev %.*s already existed", SPLAT(docID), SPLAT(newRevID));
    c4doc_release(doc);
}

string C4Test::createNewRev(C4Database* db, C4Slice docID, C4Slice body, C4RevisionFlags flags) {
    C4Collection* coll = c4db_getDefaultCollection(db, nullptr);
    Require(coll != nullptr);
    return createNewRev(coll, docID, body, flags);
}

string C4Test::createNewRev(C4Database* db, C4Slice docID, C4Slice curRevID, C4Slice body, C4RevisionFlags flags) {
    C4Collection* coll = c4db_getDefaultCollection(db, nullptr);
    Require(coll != nullptr);
    return createNewRev(coll, docID, curRevID, body, flags);
}

string C4Test::createNewRev(C4Collection* coll, C4Slice docID, C4Slice body, C4RevisionFlags flags) {
    C4Database*       db = c4coll_getDatabase(coll);
    TransactionHelper t(db);
    C4Error           error;
    auto              curDoc = c4coll_getDoc(coll, docID, false, kDocGetCurrentRev, &error);
    Require(curDoc != nullptr);
    string revID = createNewRev(coll, docID, curDoc->revID, body, flags);
    c4doc_release(curDoc);
    return revID;
}

string C4Test::createNewRev(C4Collection* coll, C4Slice docID, C4Slice curRevID, C4Slice body, C4RevisionFlags flags) {
    C4Slice history[2] = {curRevID};

    C4DocPutRequest rq = {};
    rq.docID           = docID;
    rq.history         = history;
    rq.historyCount    = (curRevID.buf != nullptr);
    rq.body            = body;
    rq.revFlags        = flags;
    rq.save            = true;
    C4Error error;
    auto    doc = c4coll_putDoc(coll, &rq, nullptr, &error);
    Require(doc);
    string revID((char*)doc->revID.buf, doc->revID.size);
    c4doc_release(doc);
    return revID;
}

string C4Test::createFleeceRev(C4Database* db, C4Slice docID, C4Slice revID, C4Slice json, C4RevisionFlags flags) {
    C4Collection* coll = c4db_getDefaultCollection(db, nullptr);
    Require(coll != nullptr);
    return createFleeceRev(coll, docID, revID, json, flags);
}

string C4Test::createFleeceRev(C4Collection* coll, C4Slice docID, C4Slice revID, C4Slice json, C4RevisionFlags flags) {
    C4Database*       db = c4coll_getDatabase(coll);
    TransactionHelper t(db);
    SharedEncoder     enc(c4db_getSharedFleeceEncoder(db));
    enc.convertJSON(json);
    fleece::alloc_slice fleeceBody = enc.finish();
    if ( !fleeceBody ) {
        Info("Encoder error " << enc.error());
        Require(fleeceBody);
    }
    if ( revID.buf ) {
        createRev(coll, docID, revID, fleeceBody, flags);
        return string(slice(revID));
    } else {
        return createNewRev(coll, docID, fleeceBody, flags);
    }
}

void C4Test::createNumberedDocs(unsigned numberOfDocs) const {
    TransactionHelper t(db);
    constexpr size_t  bufSize = 20;
    char              docID[bufSize];
    for ( unsigned i = 1; i <= numberOfDocs; i++ ) {
        snprintf(docID, bufSize, "doc-%03u", i);
        createRev(c4str(docID), kRevID, kFleeceBody);
    }
}

string C4Test::listSharedKeys(const string& delimiter) const {
    stringstream result;
    auto         sk = c4db_getFLSharedKeys(db);
    Require(sk);
    for ( int keyCode = 0; true; ++keyCode ) {
        FLSlice key = FLSharedKeys_Decode(sk, keyCode);
        if ( !key.buf ) break;
        if ( keyCode > 0 ) result << delimiter;
        result << string((char*)key.buf, key.size);
    }
    return result.str();
}

string C4Test::getDocJSON(C4Database* inDB, C4Slice docID) {
    return getDocJSON(c4db_getCollection(inDB, kC4DefaultCollectionSpec, nullptr), docID);
}

string C4Test::getDocJSON(C4Collection* collection, C4Slice docID) {
    auto doc = c4coll_getDoc(collection, docID, true, kDocGetAll, ERROR_INFO());
    Require(doc);
    fleece::alloc_slice json(c4doc_bodyAsJSON(doc, true, ERROR_INFO()));
    Require(json);
    c4doc_release(doc);
    return json.asString();
}

bool C4Test::docBodyEquals(C4Document* doc, slice fleece) const {
    Dict root = c4doc_getProperties(doc);
    if ( !root ) return false;

    Doc fleeceDoc = Doc(alloc_slice(fleece), kFLUntrusted, c4db_getFLSharedKeys(db));
    assert(fleeceDoc);
    return root.isEqual(fleeceDoc.asDict());
}

#pragma mark - ATTACHMENTS / BLOBS:

vector<C4BlobKey> C4Test::addDocWithAttachments(C4Slice docID, const vector<string>& attachments,
                                                const char* contentType, vector<string>* legacyNames,
                                                C4RevisionFlags flags) const {
    return addDocWithAttachments(db, kC4DefaultCollectionSpec, docID, attachments, contentType, legacyNames, flags);
}

vector<C4BlobKey> C4Test::addDocWithAttachments(C4Database* database, C4CollectionSpec collSpec, C4Slice docID,
                                                const vector<string>& attachments, const char* contentType,
                                                vector<string>* legacyNames, C4RevisionFlags flags) {
    C4Collection* coll = c4db_getCollection(database, collSpec, ERROR_INFO());
    Require(coll);

    vector<C4BlobKey> keys;
    stringstream      json;
    int               i = 0;
    json << (legacyNames ? "{_attachments: {" : "{attached: [");
    for ( const string& attachment : attachments ) {
        C4BlobKey key;
        Require(c4blob_create(c4db_getBlobStore(database, nullptr), fleece::slice(attachment), nullptr, &key,
                              WITH_ERROR()));
        keys.push_back(key);
        C4SliceResult keyStr = c4blob_keyToString(key);
        if ( legacyNames ) json << '"' << (*legacyNames)[i++] << "\": {";
        else
            json << "{'" << kC4ObjectTypeProperty << "': '" << kC4ObjectType_Blob << "', ";
        json << "digest: '" << string((char*)keyStr.buf, keyStr.size) << "', length: " << attachment.size()
             << ", content_type: '" << contentType << "'},";
        c4slice_free(keyStr);
    }
    json << (legacyNames ? "}}" : "]}");
    string        jsonStr = json5(json.str());
    C4SliceResult body    = c4db_encodeJSON(database, c4str(jsonStr.c_str()), ERROR_INFO());
    Require(body.buf);

    // Save document:
    C4DocPutRequest rq = {};
    rq.docID           = docID;
    rq.revFlags        = flags | kRevHasAttachments;
    rq.allocedBody     = body;
    rq.save            = true;
    C4Document* doc    = c4coll_putDoc(coll, &rq, nullptr, ERROR_INFO());
    c4slice_free(body);
    Require(doc != nullptr);
    c4doc_release(doc);
    return keys;
}

void C4Test::checkAttachment(C4Database* inDB, C4BlobKey blobKey, C4Slice expectedData) {
    C4SliceResult blob  = c4blob_getContents(c4db_getBlobStore(inDB, nullptr), blobKey, ERROR_INFO());
    auto          equal = blob == expectedData;
    Check(equal);
    c4slice_free(blob);
}

void C4Test::checkAttachments(C4Database* inDB, vector<C4BlobKey> blobKeys, vector<string> expectedData) {
    for ( unsigned i = 0; i < blobKeys.size(); ++i ) checkAttachment(inDB, blobKeys[i], fleece::slice(expectedData[i]));
}

#pragma mark - FILE IMPORT:

// Parameter is relative filepath for cert from project root
fleece::alloc_slice C4Test::readFile(const std::string& filepath) {
    std::ifstream inFile(filepath);
    Require(inFile.is_open());
    return readFile(inFile);
}

fleece::alloc_slice C4Test::readFile(std::istream& istream) {
    std::stringstream outData;
    try {  // The << operator can throw if an I/O error occured
        istream.exceptions(std::ifstream::failbit);
        outData << istream.rdbuf();
    } catch ( const std::ios_base::failure& f ) { Require(false); }
    alloc_slice result{outData.str()};
    return result;
}

bool C4Test::readFileByLines(const string& path, function_ref<bool(FLSlice)> callback, size_t maxLines) {
    return ReadFileByLines(path, callback, maxLines);
}

unsigned C4Test::importJSONFile(const string& path, const string& idPrefix, double timeout, bool verbose) const {
    C4Log("Reading %s ...  ", path.c_str());
    std::ifstream inFile(path);
    REQUIRE(inFile.is_open());
    return importJSONFile(inFile, c4db_getDefaultCollection(db, nullptr), idPrefix, timeout, verbose);
}

unsigned C4Test::importJSONFile(std::istream& istream, C4Collection* collection, const string& idPrefix, double timeout,
                                bool verbose) const {
    fleece::Stopwatch st;
    FLError           error;
    alloc_slice       fleeceData = FLData_ConvertJSON(readFile(istream), &error);
    Require(fleeceData.buf != nullptr);
    Array root = FLValue_AsArray(FLValue_FromData((C4Slice)fleeceData, kFLTrusted));
    Require(root);

    auto              database = c4coll_getDatabase(collection);
    TransactionHelper t(database);

    FLArrayIterator  iter;
    FLValue          item;
    unsigned         numDocs = 0;
    constexpr size_t bufSize = 20;
    for ( FLArrayIterator_Begin(root, &iter); nullptr != (item = FLArrayIterator_GetValue(&iter));
          (void)FLArrayIterator_Next(&iter) ) {
        char docID[bufSize];
        snprintf(docID, bufSize, "%s%07u", idPrefix.c_str(), numDocs + 1);

        FLEncoder enc = c4db_getSharedFleeceEncoder(db);
        FLEncoder_WriteValue(enc, item);
        FLSliceResult body = FLEncoder_Finish(enc, nullptr);

        // Save document:
        C4DocPutRequest rq = {};
        rq.docID           = c4str(docID);
        rq.allocedBody     = body;
        rq.save            = true;
        C4Document* doc    = c4coll_putDoc(collection, &rq, nullptr, ERROR_INFO());
        Require(doc != nullptr);
        c4doc_release(doc);
        FLSliceResult_Release(body);
        ++numDocs;
        if ( numDocs % 1000 == 0 && timeout > 0.0 && st.elapsed() >= timeout ) {
            C4Warn("Stopping JSON import after %.3f sec  ", st.elapsed());
            return false;
        }
        if ( verbose && numDocs % 100000 == 0 ) C4Log("%u  ", numDocs);
    }
    if ( verbose ) st.printReport("Importing", numDocs, "doc");
    return numDocs;
}

// Read a file that contains a JSON document per line. Every line becomes a document.
unsigned C4Test::importJSONLines(const string& path, C4Collection* collection, double timeout, bool verbose,
                                 size_t maxLines, const string& idPrefix) {
    C4Log("Reading %s ...  ", path.c_str());
    fleece::Stopwatch st;

    auto     database = c4coll_getDatabase(collection);
    uint64_t docCount = c4coll_getDocumentCount(collection);
    unsigned numDocs  = 0;
    bool     completed;
    {
        TransactionHelper t(database);
        completed = readFileByLines(
                path,
                [&](FLSlice line) {
                    fleece::alloc_slice body = c4db_encodeJSON(database, {line.buf, line.size}, ERROR_INFO());
                    Require(body.buf);

                    constexpr size_t bufSize = 80;
                    char             docID[bufSize];
                    snprintf(docID, bufSize, "%s%07u", idPrefix.c_str(), unsigned(docCount + 1));

                    // Save document:
                    C4DocPutRequest rq = {};
                    rq.docID           = c4str(docID);
                    rq.allocedBody     = {(void*)body.buf, body.size};
                    rq.save            = true;
                    C4Document* doc    = c4coll_putDoc(collection, &rq, nullptr, ERROR_INFO());
                    Require(doc != nullptr);
                    c4doc_release(doc);
                    ++numDocs;
                    ++docCount;
                    if ( numDocs % 1000 == 0 && timeout > 0.0 && st.elapsed() >= timeout ) {
                        C4Warn("Stopping JSON import after %.3f sec  ", st.elapsed());
                        return false;
                    }
                    if ( verbose && numDocs % 100000 == 0 ) C4Log("%u  ", numDocs);
                    return true;
                },
                maxLines);
        C4Log("Committing...");
    }
    if ( verbose ) st.printReport("Importing", numDocs, "doc");
    if ( completed ) Check(c4coll_getDocumentCount(collection) == docCount);
    return numDocs;
}

// Ignore "method can be made const"
// NOLINTBEGIN(readability-make-member-function-const)
unsigned C4Test::importJSONLines(const string& path, double timeout, bool verbose, C4Database* database,
                                 size_t maxLines, const string& idPrefix) {
    if ( database == nullptr ) database = db;
    return importJSONLines(path, c4db_getDefaultCollection(database, nullptr), timeout, verbose, maxLines, idPrefix);
}

// NOLINTEND(readability-make-member-function-const)

const C4Slice C4Test::kDocID = C4STR("mydoc");
C4Slice       C4Test::kFleeceBody, C4Test::kEmptyFleeceBody;
