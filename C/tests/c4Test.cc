//
//  c4Test.cc
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 9/16/15.
//  Copyright (c) 2015-2016 Couchbase. All rights reserved.
//

#include "c4Test.hh"
#include "c4Document+Fleece.h"
#include "slice.hh"
#include "Benchmark.hh"
#include <iostream>
#include <fstream>
#include <stdio.h>
#include "PlatformIO.hh"
#ifndef _MSC_VER
#include <unistd.h>
#endif

using namespace std;


std::string TempDir() {
    const char* tmpDir = getenv("TMPDIR");
    if (tmpDir == nullptr) {
#ifdef _MSC_VER
        tmpDir = "C:\\tmp";
#else
        tmpDir = "/tmp";
#endif
    }
    return string(tmpDir) + kPathSeparator;
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


ostream& operator<< (ostream& o, C4Slice s) {
    o << "C4Slice[";
    if (s.buf == nullptr)
        return o << "null]";
    auto buf = (const uint8_t*)s.buf;
    for (size_t i = 0; i < s.size; i++) {
        if (buf[i] < 32 || buf[i] > 126)
            return o << c4sliceToHex(s) << "]";
    }
    return o << '"' << string((char*)s.buf, s.size) << "\"]";
}


ostream& operator<< (ostream &out, C4Error error) {
    C4SliceResult s = c4error_getMessage(error);
    out << "C4Error(" << error.domain << ", " << error.code << "): \"" << string((const char*)s.buf, s.size) << "\"";
    c4slice_free(s);
    return out;
}


std::string json5(std::string str) {
    FLError err;
    FLSliceResult json = FLJSON5_ToJSON({str.data(), str.size()}, &err);
    REQUIRE(json.buf);
    std::string result((char*)json.buf, json.size);
    FLSliceResult_Free(json);
    return result;
}


static void log(C4LogDomain domain, C4LogLevel level, C4Slice message) {
    static const char* kLevelNames[5] = {"debug", "verbose", "info", "WARNING", "ERROR"};
    fprintf(stderr, "LiteCore-C %s %s: %.*s\n",
            c4log_getDomainName(domain),
            kLevelNames[level],
            (int)message.size, (char*)message.buf);
}


#pragma mark - C4TEST CLASS


#if defined(CMAKE) && !defined(__ANDROID__)
    #ifdef _MSC_VER
        string C4Test::sFixturesDir = "../../../../C/tests/data/";
    #else
        string C4Test::sFixturesDir = "../../../C/tests/data/";
    #endif
#else
    string C4Test::sFixturesDir = "C/tests/data/";
#endif



C4Test::C4Test(int testOption)
:_storage(kC4SQLiteStorageEngine),
 _versioning((testOption & 1) ? kC4VersionVectors : kC4RevisionTrees),
 _bundled(true)
{
    c4_shutdown(nullptr);

    objectCount = c4_getObjectCount();
    c4log_register(kC4LogWarning, log);
    c4log_setLevel(kC4DefaultLog, kC4LogInfo);

    C4DatabaseConfig config = { };
    config.flags = kC4DB_Create | kC4DB_SharedKeys;
    config.storageEngine = _storage;
    config.versioning = _versioning;

    if (_bundled)
        config.flags |= kC4DB_Bundled;

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
        FLEncoder_WriteKey(enc, FLSTR("answer"));
        FLEncoder_WriteInt(enc, 42);
        FLEncoder_EndDict(enc);
        auto result = FLEncoder_Finish(enc, nullptr);
        kFleeceBody = {result.buf, result.size};
    }

#if 0
    if (testOption & 4) {
        config.encryptionKey.algorithm = kC4EncryptionAES256;
        memcpy(config.encryptionKey.bytes, "this is not a random key at all.", 32);
    }
#endif
    static C4DatabaseConfig sLastConfig = { };
    if (config.flags != sLastConfig.flags || config.versioning != sLastConfig.versioning) {
        fprintf(stderr, "            %s, %s\n",
                config.storageEngine,
                (config.versioning==kC4VersionVectors ? "version-vectors" : "rev-trees"));
        sLastConfig = config;
    }

    _dbPath = TempDir();
    if (_bundled)
        _dbPath += "cbl_core_test";
    else if (storageType() == kC4SQLiteStorageEngine)
        _dbPath += "cbl_core_test.sqlite3";
    else {
        FAIL("Unknown storage type");
    }

    C4Error error;
    if (!c4db_deleteAtPath(databasePath(), &config, &error))
        REQUIRE(error.code == 0);
    db = c4db_open(databasePath(), &config, &error);
    REQUIRE(db != nullptr);
}


C4Test::~C4Test() {
    C4Error error;
    c4db_delete(db, &error);
    c4db_free(db);

#if ATOMIC_INT_LOCK_FREE > 1
    if (!current_exception()) {
        // Check for leaks:
        REQUIRE(c4_getObjectCount() == objectCount);
    }
#endif
}


void C4Test::reopenDB() {
    auto config = *c4db_getConfig(db);
    C4Error error;
    REQUIRE(c4db_close(db, &error));
    c4db_free(db);
    db = nullptr;
    db = c4db_open(databasePath(), &config, &error);
    REQUIRE(db);
}


void C4Test::createRev(C4Slice docID, C4Slice revID, C4Slice body, C4RevisionFlags flags) {
    C4Test::createRev(db, docID, revID, body, flags);
}

void C4Test::createRev(C4Database *db, C4Slice docID, C4Slice revID, C4Slice body, C4RevisionFlags flags) {
    TransactionHelper t(db);
    C4Error error;
    auto curDoc = c4doc_get(db, docID, false, &error);
    REQUIRE(curDoc != nullptr);

    C4Slice history[2] = {revID, curDoc->revID};

    C4DocPutRequest rq = {};
    rq.existingRevision = true;
    rq.docID = docID;
    rq.history = history;
    rq.historyCount = 1 + (curDoc->revID.buf != nullptr);
    rq.body = body;
    rq.revFlags = flags;
    rq.save = true;
    auto doc = c4doc_put(db, &rq, nullptr, &error);
    REQUIRE(doc != nullptr);
    c4doc_free(doc);
    c4doc_free(curDoc);
}


void C4Test::createNumberedDocs(unsigned numberOfDocs) {
    char docID[20];
    for (unsigned i = 1; i <= numberOfDocs; i++) {
        sprintf(docID, "doc-%03u", i);
        createRev(c4str(docID), kRevID, kBody);
    }
}


string C4Test::listSharedKeys(string delimiter) {
    stringstream result;
    auto sk = c4db_getFLSharedKeys(db);
    REQUIRE(sk);
    for (int keyCode = 0; true; ++keyCode) {
        FLSlice key = FLSharedKey_GetKeyString(sk, keyCode, nullptr);
        if (!key.buf)
            break;
        if (keyCode > 0)
            result << delimiter;
        result << string((char*)key.buf, key.size);
    }
    return result.str();
}


#pragma mark - FILE IMPORT:


// Reads a file into memory.
FLSlice C4Test::readFile(std::string path) {
    INFO("Opening file " << path);
    FILE *fd = fopen(path.c_str(), "rb");
    REQUIRE(fd != nullptr);
    fseeko(fd, 0, SEEK_END);
    auto size = (size_t)ftello(fd);
    fseeko(fd, 0, SEEK_SET);
    void* data = malloc(size);
    REQUIRE(data);
    ssize_t bytesRead = fread((void*)data, 1, size, fd);
    REQUIRE(bytesRead == size);
    fclose(fd);
    return FLSlice{data, size};
}


bool C4Test::readFileByLines(string path, function<bool(FLSlice)> callback) {
    INFO("Reading lines from " << path);
    fstream fd(path.c_str(), ios_base::in);
    REQUIRE(fd);
    char buf[1000000];  // The Wikipedia dumps have verrry long lines
    while (fd.good()) {
        fd.getline(buf, sizeof(buf));
        auto len = fd.gcount();
        if (len <= 0)
            break;
        REQUIRE(buf[len-1] == '\0');
        --len;
        if (!callback({buf, (size_t)len}))
            return false;
    }
    REQUIRE(fd.eof());
    return true;
}


unsigned C4Test::importJSONFile(string path, string idPrefix, double timeout, bool verbose) {
    C4Log("Reading %s ...  ", path.c_str());
    Stopwatch st;
    auto jsonData = readFile(path);
    FLError error;
    FLSliceResult fleeceData = FLData_ConvertJSON({jsonData.buf, jsonData.size}, &error);
    REQUIRE(fleeceData.buf != nullptr);
    free((void*)jsonData.buf);
    Array root = FLValue_AsArray(FLValue_FromTrustedData((C4Slice)fleeceData));
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

        FLEncoder enc = c4db_createFleeceEncoder(db);
        FLEncoder_WriteValue(enc, item);
        FLSliceResult body = FLEncoder_Finish(enc, nullptr);

        // Save document:
        C4Error c4err;
        C4DocPutRequest rq = {};
        rq.docID = c4str(docID);
        rq.body = (C4Slice)body;
        rq.save = true;
        C4Document *doc = c4doc_put(db, &rq, nullptr, &c4err);
        REQUIRE(doc != nullptr);
        c4doc_free(doc);
        FLSliceResult_Free(body);
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
unsigned C4Test::importJSONLines(string path, double timeout, bool verbose) {
    C4Log("Reading %s ...  ", path.c_str());
    Stopwatch st;
    unsigned numDocs = 0;
    {
        TransactionHelper t(db);
        readFileByLines(path, [&](FLSlice line)
        {
            C4Error c4err;
            FLError error;
            FLSliceResult body = c4db_encodeJSON(db, {line.buf, line.size}, &c4err);
            REQUIRE(body.buf);

            char docID[20];
            sprintf(docID, "%07u", numDocs+1);

            // Save document:
            C4DocPutRequest rq = {};
            rq.docID = c4str(docID);
            rq.body = (C4Slice)body;
            rq.save = true;
            C4Document *doc = c4doc_put(db, &rq, nullptr, &c4err);
            REQUIRE(doc != nullptr);
            c4doc_free(doc);
            FLSliceResult_Free(body);
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
const C4Slice C4Test::kBody  = C4STR("{\"name\":007}");
C4Slice C4Test::kFleeceBody;
