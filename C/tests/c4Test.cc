//
//  c4Test.cc
//  CBForest
//
//  Created by Jens Alfke on 9/16/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//

#include "c4Test.hh"
#include "slice.hh"
#include <iostream>
#ifndef _MSC_VER
#include <unistd.h>
#endif


// Debugging utility to print a slice -- in LLDB enter "call ps(___)"
void ps(C4Slice s);
void ps(C4Slice s) {
    std::cerr << s << "\n";
}

void ps(fleece::slice s);
void ps(fleece::slice s) {
    ps(C4Slice{s.buf, s.size});

}

bool operator== (C4Slice s1, C4Slice s2) {
    return s1.size == s2.size && memcmp(s1.buf, s2.buf, s1.size) == 0;
}

std::ostream& operator<< (std::ostream& o, C4Slice s) {
    o << "C4Slice[";
    if (s.buf == NULL)
        return o << "null]";
    auto buf = (const uint8_t*)s.buf;
    for (size_t i = 0; i < s.size; i++) {
        if (buf[i] < 32 || buf[i] > 126)
            return o << "binary, " << s.size << " bytes]";
    }
    return o << '"' << std::string((char*)s.buf, s.size) << "\"]";
}


std::ostream& operator<< (std::ostream &out, C4Error error) {
    C4SliceResult s = c4error_getMessage(error);
    out << "C4Error(" << error.domain << ", " << error.code << "): \"" << std::string((const char*)s.buf, s.size) << "\"";
    c4slice_free(s);
    return out;
}


// Dumps a C4Key to a C++ string
std::string toJSON(C4KeyReader r) {
    C4SliceResult dump = c4key_toJSON(&r);
    std::string result((char*)dump.buf, dump.size);
    c4slice_free(dump);
    return result;
}


static void log(C4LogLevel level, C4Slice message) {
    static const char* kLevelNames[4] = {"debug", "info", "WARNING", "ERROR"};
    fprintf(stderr, "CBForest-C %s: %*s\n", kLevelNames[level], (int)message.size, message.buf);
}


#pragma mark - C4TEST CLASS


C4Slice C4Test::databasePath() {
    if (storageType() == kC4SQLiteStorageEngine)
        return c4str(kTestDir "cbforest_test.sqlite3");
    else
        return c4str(kTestDir "cbforest_test.forestdb");
}


C4Test::C4Test() {
    c4_shutdown(NULL);
    
    objectCount = c4_getObjectCount();
    c4log_register(kC4LogWarning, log);

    C4DatabaseConfig config = { };
    config.flags = kC4DB_Create;
    config.storageEngine = storageType();

    if (schemaVersion() == 1) {
        kRevID = C4STR("1-abcd");
        kRev2ID = C4STR("2-c001d00d");
    } else {
        config.flags |= kC4DB_V2Format;
        kRevID = C4STR("1@*");
        kRev2ID = C4STR("2@c001d00d");
    }

    if (encryptionKey())
        config.encryptionKey = *encryptionKey();

    static C4DatabaseConfig sLastConfig = { };
    if (config.flags != sLastConfig.flags || config.storageEngine != config.storageEngine) {
        fprintf(stderr, "Using db %s storage, flags 0x%04x\n", config.storageEngine, config.flags);
        sLastConfig = config;
    }

    C4Error error;
    c4db_deleteAtPath(databasePath(), &config, NULL);
    db = c4db_open(databasePath(), &config, &error);
    REQUIRE(db != NULL);
}


C4Test::~C4Test() {
    C4Error error;
    c4db_delete(db, &error);
    c4db_free(db);

    // Check for leaks:
    REQUIRE(c4_getObjectCount() == objectCount);
}


void C4Test::createRev(C4Slice docID, C4Slice revID, C4Slice body, bool isNew) {
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
    rq.deletion = (body.buf == nullptr);
    rq.save = true;
    auto doc = c4doc_put(db, &rq, NULL, &error);
    REQUIRE(doc != NULL);
    c4doc_free(doc);
    c4doc_free(curDoc);
}


const C4Slice C4Test::kDocID = C4STR("mydoc");
const C4Slice C4Test::kBody  = C4STR("{\"name\":007}");
