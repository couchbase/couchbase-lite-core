//
// c4Test.cc
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
#include "c4BlobStore.h"
#include "c4Document+Fleece.h"
#include "c4Private.h"
#include "fleece/slice.hh"
#include "FilePath.hh"
#include "StringUtil.hh"
#include "Backtrace.hh"
#include "Benchmark.hh"
#include <csignal>
#include <iostream>
#include <fstream>
#include <stdio.h>
#include "PlatformIO.hh"
#include <thread>
#include <mutex>
#include <chrono>

#if TARGET_OS_IPHONE
#include <CoreFoundation/CFBundle.h>
#endif

using namespace std;

const std::string& TempDir() {
    static string kTempDir;

    static once_flag f;
    call_once(f, [=] {
        char folderName[64];
        sprintf(folderName, "LiteCore_C_Tests%lld/", chrono::milliseconds(time(nullptr)).count());
        auto temp = litecore::FilePath::tempDirectory()[folderName];
        temp.mkdir();
        kTempDir = temp.path();
    });

    return kTempDir;
}


// Debugging utility to print a slice -- in LLDB enter "call ps(___)"
void ps(C4Slice s);
void ps(C4Slice s) {
    cerr << s << "\n";
}

void ps(fleece::slice s);
void ps(fleece::slice s) {
    ps(C4Slice{s.buf, s.size});

}


static string c4sliceToHex(C4Slice result) {
    string hex;
    for (size_t i = 0; i < result.size; i++) {
        char str[4];
        sprintf(str, "%02X", ((const uint8_t*)result.buf)[i]);
        hex.append(str);
        if ((i % 4) == 3 && i != result.size-1)
            hex.append(" ");
    }
    return hex;
}


ostream& operator<< (ostream& o, fleece::slice s) {
    o << "slice[";
    if (s.buf == nullptr)
        return o << "null]";
    auto buf = (const uint8_t*)s.buf;
    for (size_t i = 0; i < s.size; i++) {
        if (buf[i] < 32 || buf[i] > 126)
            return o << c4sliceToHex(s) << "]";
    }
    return o << '"' << string((char*)s.buf, s.size) << "\"]";
}


ostream& operator<< (ostream& o, fleece::alloc_slice s)         {return o << fleece::slice(s.buf, s.size);}
std::ostream& operator<< (std::ostream& o, C4Slice s)           {return o << fleece::slice(s);}
std::ostream& operator<< (std::ostream& o, C4SliceResult s)     {return o << fleece::slice(s.buf,s.size);}



ostream& operator<< (ostream &out, C4Error error) {
    C4SliceResult s = c4error_getDescription(error);
    out << "C4Error(" << string((const char*)s.buf, s.size) << ")";
    c4slice_free(s);
    return out;
}


fleece::alloc_slice json5slice(std::string str) {
    FLStringResult errorMsg = {};
    size_t errorPos = 0;
    FLError err;
    FLSliceResult json = FLJSON5_ToJSON(slice(str), &errorMsg, &errorPos, &err);
    INFO("JSON5 error: " << string(alloc_slice(errorMsg)) << ", input was: " << str);
    REQUIRE(json.buf);
    return json;
}


std::string json5(std::string str) {
    return string(json5slice(str));
}


//static void log(C4LogDomain domain, C4LogLevel level, C4Slice message) {
//    static const char* kLevelNames[5] = {"debug", "verbose", "info", "WARNING", "ERROR"};
//    fprintf(stderr, "LiteCore-C %s %s: %.*s\n",
//            c4log_getDomainName(domain),
//            kLevelNames[level],
//            (int)message.size, (char*)message.buf);
//}


void AssertionFailed(const char *fn, const char *file, unsigned line, const char *expr,
                     const char *message)
{
    if (!message)
        message = expr;
    fprintf(stderr, "FATAL: Assertion failed: %s (%s:%u, in %s)\n", message, file, line, fn);
    abort();
}


void CheckError(C4Error error,
                C4ErrorDomain expectedDomain, int expectedCode, const char *expectedMessage)
{
    alloc_slice desc = c4error_getDescription(error);
    INFO("Error is " << string(desc));
    CHECK(error.domain == expectedDomain);
    CHECK(error.code == expectedCode);
    if (expectedMessage) {
        C4StringResult msg = c4error_getMessage(error);
        CHECK(string((char*)msg.buf, msg.size) == string(expectedMessage));
        c4slice_free(msg);
    }
}


void WaitUntil(int timeoutMillis, function_ref<bool()> predicate) {
    for (int remaining = timeoutMillis; remaining >= 0; remaining -= 100) {
        if (predicate())
            return;
        this_thread::sleep_for(chrono::milliseconds(100));
    }
    FAIL("Wait timed out after " << timeoutMillis << "ms");
}


#pragma mark - C4TEST CLASS

#if defined(CMAKE) && defined(_MSC_VER)
string C4Test::sFixturesDir = "../C/tests/data/";
string C4Test::sReplicatorFixturesDir = "../Replicator/tests/data/";
#else
string C4Test::sFixturesDir = "C/tests/data/";
string C4Test::sReplicatorFixturesDir = "Replicator/tests/data/";
#endif

const string C4Test::kDatabaseName = "cbl_core_test";


C4Test::C4Test(int testOption)
:_storage(kC4SQLiteStorageEngine),
#if ENABLE_VERSION_VECTORS
 _versioning((testOption > 1) ? kC4VersionVectors : kC4RevisionTrees)
#else
_versioning(kC4RevisionTrees)
#endif
{
    static once_flag once;
    call_once(once, [] {
        c4log_enableFatalExceptionBacktrace();

        fleece::alloc_slice buildInfo = c4_getBuildInfo();
        fleece::alloc_slice version = c4_getVersion();
        C4Log("This is LiteCore %.*s ... short version %.*s", SPLAT(buildInfo), SPLAT(version));

        if (c4log_binaryFileLevel() == kC4LogNone) {
            string path = TempDir() + "LiteCoreAPITests";
            C4Log("Beginning binary logging to %s", path.c_str());
            C4Error error;
            REQUIRE(c4log_writeToBinaryFile({kC4LogVerbose, c4str(path.c_str()), 16*1024, 1, false}, &error));
        }
        //c4log_setBinaryFileLevel(kC4LogDebug);
        if (getenv("LiteCoreTestsQuiet"))
            c4log_setCallbackLevel(kC4LogWarning);

#if TARGET_OS_IPHONE
        static once_flag once;
        call_once(once, [] {
            // iOS tests copy the fixture files into the test bundle.
            CFBundleRef bundle = CFBundleGetBundleWithIdentifier(CFSTR("org.couchbase.LiteCoreTests"));
            CFURLRef url = CFBundleCopyResourcesDirectoryURL(bundle);
            CFStringRef path = CFURLCopyPath(url);
            char resourcesDir[1024];
            Assert(CFStringGetCString(path, resourcesDir, sizeof(resourcesDir), kCFStringEncodingUTF8));
            sFixturesDir           = string(resourcesDir) + "TestData/C/tests/data/";
            sReplicatorFixturesDir = string(resourcesDir) + "TestData/Replicator/tests/data/";
            CFRelease(path);
            CFRelease(url);
        });
#endif
    });
    c4log_warnOnErrors(true);

    c4_shutdown(nullptr);

    objectCount = c4_getObjectCount();

    C4DatabaseConfig config = { };
    config.flags = kC4DB_Create;
    config.storageEngine = _storage;
    config.versioning = _versioning;

    if (config.versioning == kC4RevisionTrees) {
        kRevID = C4STR("1-abcd");
        kRev2ID = C4STR("2-c001d00d");
        kRev3ID = C4STR("3-deadbeef");
    } else {
        kRevID = C4STR("1@*");
        kRev2ID = C4STR("2@*");
        kRev3ID = C4STR("3@*");
    }

    if (!kFleeceBody.buf) {
        auto enc = FLEncoder_New();
        FLEncoder_BeginDict(enc, 1);
        FLEncoder_WriteKey(enc, FLSTR("ans*wer"));
        FLEncoder_WriteInt(enc, 42);
        FLEncoder_EndDict(enc);
        auto result = FLEncoder_Finish(enc, nullptr);
        kFleeceBody = {result.buf, result.size};
        FLEncoder_Free(enc);
    }

    if (!kEmptyFleeceBody.buf) {
        auto enc = FLEncoder_New();
        FLEncoder_BeginDict(enc, 1);
        FLEncoder_EndDict(enc);
        auto result = FLEncoder_Finish(enc, nullptr);
        kEmptyFleeceBody = {result.buf, result.size};
        FLEncoder_Free(enc);
    }

    if (testOption & 1) {
        config.encryptionKey.algorithm = kC4EncryptionAES256;
        memcpy(config.encryptionKey.bytes, "this is not a random key at all.", kC4EncryptionKeySizeAES256);
    }

    static C4DatabaseConfig sLastConfig = { };
    if (config.flags != sLastConfig.flags || config.versioning != sLastConfig.versioning
                    || config.encryptionKey.algorithm != sLastConfig.encryptionKey.algorithm) {
        fprintf(stderr, "        --- %s, %s%s\n",
                config.storageEngine,
                (config.versioning==kC4RevisionTrees ? "rev-trees" : "?unknown versioning?"),
                (config.encryptionKey.algorithm ? ", encrypted" : ""));
        sLastConfig = config;
    }

    _dbPath = TempDir() + kDatabaseName + kC4DatabaseFilenameExtension;

    C4Error error;
    if (!c4db_deleteAtPath(databasePath(), &error))
        REQUIRE(error.code == 0);
    db = c4db_open(databasePath(), &config, &error);
    INFO("Error " << error.domain << "/" << error.code);
    REQUIRE(db != nullptr);
}

C4Test::~C4Test() {
    if (db)
        deleteDatabase();

#if ATOMIC_INT_LOCK_FREE > 1
    if (!current_exception()) {
        // Check for leaks:
        int leaks;
        int attempt = 0;
        while ((leaks = c4_getObjectCount() - objectCount) > 0 && attempt++ < 10) {
            this_thread::sleep_for(chrono::microseconds(200000)); // wait up to 2 seconds for bg threads to free objects
        }
        if (leaks > 0) {
            fprintf(stderr, "*** LEAKED LITECORE OBJECTS: \n");
            c4_dumpInstances();
            fprintf(stderr, "***\n");
        }
        CHECK(leaks == 0);
    }
#endif
}


C4Database* C4Test::createDatabase(const string &nameSuffix) {
    REQUIRE(!nameSuffix.empty());
    string dbPath = fleece::slice(databasePath()).asString();
    Assert(litecore::hasSuffix(dbPath, kC4DatabaseFilenameExtension));
    dbPath.replace(dbPath.size()-8, 8, "_" + nameSuffix + kC4DatabaseFilenameExtension);
    auto dbPathSlice = c4str(dbPath.c_str());

    auto config = c4db_getConfig(db);
    C4Error error;
    if (!c4db_deleteAtPath(dbPathSlice, &error))
        REQUIRE(error.code == 0);
    auto newDB = c4db_open(dbPathSlice, config, &error);
    REQUIRE(newDB != nullptr);
    return newDB;
}


void C4Test::closeDB() {
    C4Error error;
    REQUIRE(c4db_close(db, &error));
    c4db_release(db);
    db = nullptr;
}


void C4Test::reopenDB() {
    auto config = *c4db_getConfig(db);
    closeDB();
    C4Error error;
    db = c4db_open(databasePath(), &config, &error);
    REQUIRE(db);
}


void C4Test::reopenDBReadOnly() {
    auto config = *c4db_getConfig(db);
    C4Error error;
    REQUIRE(c4db_close(db, &error));
    c4db_release(db);
    db = nullptr;
    config.flags = (config.flags & ~kC4DB_Create) | kC4DB_ReadOnly;
    db = c4db_open(databasePath(), &config, &error);
    REQUIRE(db);
}


void C4Test::deleteDatabase(){
    C4Error error = {};
    bool deletedDb = c4db_delete(db, &error);
    INFO("Error " << error.domain << "/" << error.code);
    REQUIRE(deletedDb);
    c4db_release(db);
    db = nullptr;
}


void C4Test::deleteAndRecreateDB(C4Database* &db) {
    C4SliceResult path = c4db_getPath(db);
    auto config = *c4db_getConfig(db);
    C4Error error;
    REQUIRE(c4db_delete(db, &error));
    c4db_release(db);
    db = nullptr;
    db = c4db_open({path.buf, path.size}, &config, &error);
    REQUIRE(db);
    c4slice_free(path);
}


/*static*/ alloc_slice C4Test::copyFixtureDB(const string &name) {
    auto srcPath = litecore::FilePath(sFixturesDir + name, "");
    auto dbPath = litecore::FilePath::tempDirectory()[srcPath.fileOrDirName() + "/"];
    dbPath.delRecursive();
    srcPath.copyTo(dbPath);
    return alloc_slice(string(dbPath));
}


void C4Test::createRev(C4Slice docID, C4Slice revID, C4Slice body, C4RevisionFlags flags) {
    C4Test::createRev(db, docID, revID, body, flags);
}

void C4Test::createRev(C4Database *db, C4Slice docID, C4Slice revID, C4Slice body, C4RevisionFlags flags) {
    TransactionHelper t(db);
    C4Error error;
    auto curDoc = c4doc_get(db, docID, false, &error);
    REQUIRE(curDoc != nullptr);
    createConflictingRev(db, docID, curDoc->revID, revID, body, flags);
    c4doc_release(curDoc);
}

void C4Test::createConflictingRev(C4Database *db,
                                  C4Slice docID,
                                  C4Slice parentRevID,
                                  C4Slice newRevID,
                                  C4Slice body,
                                  C4RevisionFlags flags)
{
    C4Slice history[2] = {newRevID, parentRevID};
    C4DocPutRequest rq = {};
    rq.existingRevision = true;
    rq.allowConflict = true;
    rq.docID = docID;
    rq.history = history;
    rq.historyCount = 1 + (parentRevID.buf != nullptr);
    rq.body = body;
    rq.revFlags = flags;
    rq.save = true;
    C4Error error;
    auto doc = c4doc_put(db, &rq, nullptr, &error);
    char buf[256];
//    INFO("Error: " << c4error_getDescriptionC(error, buf, sizeof(buf)));
//    REQUIRE(doc != nullptr);        // can't use Catch on bg threads
    Assert(doc != nullptr);
    c4doc_release(doc);
}


string C4Test::createNewRev(C4Database *db, C4Slice docID, C4Slice body, C4RevisionFlags flags) {
    TransactionHelper t(db);
    C4Error error;
    auto curDoc = c4doc_get(db, docID, false, &error);
//    REQUIRE(curDoc != nullptr);        // can't use Catch on bg threads
    Assert(curDoc != nullptr);
    string revID = createNewRev(db, docID, curDoc->revID, body, flags);
    c4doc_release(curDoc);
    return revID;
}

string C4Test::createNewRev(C4Database *db, C4Slice docID, C4Slice curRevID, C4Slice body, C4RevisionFlags flags) {
    C4Slice history[2] = {curRevID};

    C4DocPutRequest rq = {};
    rq.docID = docID;
    rq.history = history;
    rq.historyCount = (curRevID.buf != nullptr);
    rq.body = body;
    rq.revFlags = flags;
    rq.save = true;
    C4Error error;
    auto doc = c4doc_put(db, &rq, nullptr, &error);
    if (!doc) {
        char buf[256];
        //INFO("Error: " << c4error_getDescriptionC(error, buf, sizeof(buf)));
    }
    //REQUIRE(doc != nullptr);        // can't use Catch on bg threads
    Assert(doc != nullptr);
    string revID((char*)doc->revID.buf, doc->revID.size);
    c4doc_release(doc);
    return revID;
}


string C4Test::createFleeceRev(C4Database *db, C4Slice docID, C4Slice revID, C4Slice json,
                             C4RevisionFlags flags)
{
    TransactionHelper t(db);
    SharedEncoder enc(c4db_getSharedFleeceEncoder(db));
    enc.convertJSON(json);
    fleece::alloc_slice fleeceBody = enc.finish();
//    INFO("Encoder error " << enc.error());        // can't use Catch on bg threads
//    REQUIRE(fleeceBody);
    Assert(fleeceBody);
    if (revID.buf) {
        createRev(db, docID, revID, fleeceBody, flags);
        return string(slice(revID));
    } else {
        return createNewRev(db, docID, fleeceBody, flags);
    }
}


void C4Test::createNumberedDocs(unsigned numberOfDocs) {
    TransactionHelper t(db);
    char docID[20];
    for (unsigned i = 1; i <= numberOfDocs; i++) {
        sprintf(docID, "doc-%03u", i);
        createRev(c4str(docID), kRevID, kFleeceBody);
    }
}


string C4Test::listSharedKeys(string delimiter) {
    stringstream result;
    auto sk = c4db_getFLSharedKeys(db);
    REQUIRE(sk);
    for (int keyCode = 0; true; ++keyCode) {
        FLSlice key = FLSharedKeys_Decode(sk, keyCode);
        if (!key.buf)
            break;
        if (keyCode > 0)
            result << delimiter;
        result << string((char*)key.buf, key.size);
    }
    return result.str();
}


string C4Test::getDocJSON(C4Database* inDB, C4Slice docID) {
    C4Error error;
    auto doc = c4doc_get(inDB, docID, true, &error);
    REQUIRE(doc);
    fleece::alloc_slice json( c4doc_bodyAsJSON(doc, true, &error) );
    REQUIRE(json);
    c4doc_release(doc);
    return json.asString();
}


#pragma mark - ATTACHMENTS / BLOBS:


vector<C4BlobKey> C4Test::addDocWithAttachments(C4Slice docID,
                                                vector<string> attachments,
                                                const char *contentType,
                                                vector<string>* legacyNames,
                                                C4RevisionFlags flags)
{
    vector<C4BlobKey> keys;
    C4Error c4err;
    stringstream json;
    int i = 0;
    json << (legacyNames ? "{_attachments: {" : "{attached: [");
    for (string &attachment : attachments) {
        C4BlobKey key;
        REQUIRE(c4blob_create(c4db_getBlobStore(db, nullptr), fleece::slice(attachment),
                              nullptr, &key,  &c4err));
        keys.push_back(key);
        C4SliceResult keyStr = c4blob_keyToString(key);
        if (legacyNames)
            json << '"' << (*legacyNames)[i++] << "\": {";
        else
            json << "{'" << kC4ObjectTypeProperty << "': '" << kC4ObjectType_Blob << "', ";
        json << "digest: '" << string((char*)keyStr.buf, keyStr.size)
             << "', length: " << attachment.size()
             << ", content_type: '" << contentType << "'},";
        c4slice_free(keyStr);
    }
    json << (legacyNames ? "}}" : "]}");
    string jsonStr = json5(json.str());
    C4SliceResult body = c4db_encodeJSON(db, c4str(jsonStr.c_str()), &c4err);
    REQUIRE(body.buf);

    // Save document:
    C4DocPutRequest rq = {};
    rq.docID = docID;
    rq.revFlags = flags | kRevHasAttachments;
    rq.allocedBody = body;
    rq.save = true;
    C4Document* doc = c4doc_put(db, &rq, nullptr, &c4err);
    c4slice_free(body);
    REQUIRE(doc != nullptr);
    c4doc_release(doc);
    return keys;
}

void C4Test::checkAttachment(C4Database *inDB, C4BlobKey blobKey, C4Slice expectedData) {
    C4Error c4err;
    C4SliceResult blob = c4blob_getContents(c4db_getBlobStore(inDB, nullptr), blobKey, &c4err);
    CHECK(blob == expectedData);
    c4slice_free(blob);
}

void C4Test::checkAttachments(C4Database *inDB, vector<C4BlobKey> blobKeys, vector<string> expectedData) {
    for (unsigned i = 0; i < blobKeys.size(); ++i)
        checkAttachment(inDB, blobKeys[i], fleece::slice(expectedData[i]));
}

#pragma mark - FILE IMPORT:


// Reads a file into memory.
fleece::alloc_slice C4Test::readFile(std::string path) {
    INFO("Opening file " << path);
    FILE *fd = fopen(path.c_str(), "rb");
    REQUIRE(fd != nullptr);
    fseeko(fd, 0, SEEK_END);
    auto size = (size_t)ftello(fd);
    fseeko(fd, 0, SEEK_SET);
    fleece::alloc_slice result(size);
    ssize_t bytesRead = fread((void*)result.buf, 1, size, fd);
    REQUIRE(bytesRead == size);
    fclose(fd);
    return result;
}


bool C4Test::readFileByLines(string path, function<bool(FLSlice)> callback) {
    INFO("Reading lines from " << path);
    fstream fd(path.c_str(), ios_base::in);
    REQUIRE(fd);
    vector<char> buf(1000000);  // The Wikipedia dumps have verrry long lines
    while (fd.good()) {
        fd.getline(buf.data(), buf.capacity());
        auto len = fd.gcount();
        if (len <= 0)
            break;
        REQUIRE(buf[len-1] == '\0');
        --len;
        if (!callback({buf.data(), (size_t)len}))
            return false;
    }
    REQUIRE(fd.eof());
    return true;
}


unsigned C4Test::importJSONFile(string path, string idPrefix, double timeout, bool verbose) {
    C4Log("Reading %s ...  ", path.c_str());
    fleece::Stopwatch st;
    FLError error;
    FLSliceResult fleeceData = FLData_ConvertJSON(readFile(path), &error);
    REQUIRE(fleeceData.buf != nullptr);
    Array root = FLValue_AsArray(FLValue_FromData((C4Slice)fleeceData, kFLTrusted));
    REQUIRE(root);

    TransactionHelper t(db);

    FLArrayIterator iter;
    FLValue item;
    unsigned numDocs = 0;
    for(FLArrayIterator_Begin(root, &iter);
            nullptr != (item = FLArrayIterator_GetValue(&iter));
            FLArrayIterator_Next(&iter))
    {
        char docID[20];
        sprintf(docID, "%s%07u", idPrefix.c_str(), numDocs+1);

        FLEncoder enc = c4db_getSharedFleeceEncoder(db);
        FLEncoder_WriteValue(enc, item);
        FLSliceResult body = FLEncoder_Finish(enc, nullptr);

        // Save document:
        C4Error c4err;
        C4DocPutRequest rq = {};
        rq.docID = c4str(docID);
        rq.allocedBody = body;
        rq.save = true;
        C4Document *doc = c4doc_put(db, &rq, nullptr, &c4err);
        REQUIRE(doc != nullptr);
        c4doc_release(doc);
        FLSliceResult_Release(body);
        ++numDocs;
        if (numDocs % 1000 == 0 && st.elapsed() >= timeout) {
            C4Warn("Stopping JSON import after %.3f sec  ", st.elapsed());
            return false;
        }
        if (verbose && numDocs % 100000 == 0)
            C4Log("%u  ", numDocs);
    }
    if (verbose) st.printReport("Importing", numDocs, "doc");
    return numDocs;
}


// Read a file that contains a JSON document per line. Every line becomes a document.
unsigned C4Test::importJSONLines(string path, double timeout, bool verbose, C4Database* database) {
    C4Log("Reading %s ...  ", path.c_str());
    fleece::Stopwatch st;
    if(database == nullptr) {
        database = db;
    }
    
    unsigned numDocs = 0;
    {
        TransactionHelper t(database);
        readFileByLines(path, [&](FLSlice line)
        {
            C4Error c4err;
            fleece::alloc_slice body = c4db_encodeJSON(database, {line.buf, line.size}, &c4err);
            REQUIRE(body.buf);

            char docID[20];
            sprintf(docID, "%07u", numDocs+1);

            // Save document:
            C4DocPutRequest rq = {};
            rq.docID = c4str(docID);
            rq.allocedBody = {(void*)body.buf, body.size};
            rq.save = true;
            C4Document *doc = c4doc_put(database, &rq, nullptr, &c4err);
            REQUIRE(doc != nullptr);
            c4doc_release(doc);
            ++numDocs;
            if (numDocs % 1000 == 0 && st.elapsed() >= timeout) {
                C4Warn("Stopping JSON import after %.3f sec  ", st.elapsed());
                return false;
            }
            if (verbose && numDocs % 100000 == 0)
                C4Log("%u  ", numDocs);
            return true;
        });
        C4Log("Committing...");
    }
    if (verbose) st.printReport("Importing", numDocs, "doc");
    return numDocs;
}


const C4Slice C4Test::kDocID = C4STR("mydoc");
C4Slice C4Test::kFleeceBody, C4Test::kEmptyFleeceBody;
