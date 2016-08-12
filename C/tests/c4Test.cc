//
//  c4Test.cc
//  CBForest
//
//  Created by Jens Alfke on 9/16/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//

#include "c4Test.hh"
#include "slice.hh"
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


static void log(C4LogLevel level, C4Slice message) {
    static const char* kLevelNames[4] = {"debug", "info", "WARNING", "ERROR"};
    fprintf(stderr, "CBForest-C %s: %*s\n", kLevelNames[level], (int)message.size, message.buf);
}


C4Slice C4Test::databasePath() {
    return c4str(kTestDir "cbforest_test.db");
}


void C4Test::setUp() {
    c4_shutdown(NULL);
    
    objectCount = c4_getObjectCount();
    c4log_register(kC4LogWarning, log);

    C4DatabaseFlags flags = kC4DB_Create | storageType();
    if (schemaVersion() == 1) {
        kRevID = C4STR("1-abcd");
        kRev2ID = C4STR("2-c001d00d");
    } else {
        flags |= kC4DB_V2Format;
        kRevID = C4STR("1@*");
        kRev2ID = C4STR("2@c001d00d");
    }

    static C4DatabaseFlags sLastFlags = (C4DatabaseFlags)-1;
    if (flags != sLastFlags) {
        fprintf(stderr, "Using db flags 0x%04x\n", flags);
        sLastFlags = flags;
    }

    C4Error error;
    c4db_deleteAtPath(databasePath(), flags, NULL);
    db = c4db_open(databasePath(),
                   flags,
                   encryptionKey(),
                   &error);
    Assert(db != NULL);
}


void C4Test::tearDown() {
    C4Error error;
    c4db_delete(db, &error);
    c4db_free(db);

    // Check for leaks:
    AssertEqual(c4_getObjectCount() - objectCount, 0);
}


void C4Test::createRev(C4Slice docID, C4Slice revID, C4Slice body, bool isNew) {
    TransactionHelper t(db);
    C4Error error;
    auto curDoc = c4doc_get(db, docID, false, &error);
    Assert(curDoc);

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
    Assert(doc != NULL);
    c4doc_free(doc);
    c4doc_free(curDoc);
}


const C4Slice C4Test::kDocID = C4STR("mydoc");
const C4Slice C4Test::kBody  = C4STR("{\"name\":007}");


// Dumps a C4Key to a C++ string
std::string toJSON(C4KeyReader r) {
    C4SliceResult dump = c4key_toJSON(&r);
    std::string result((char*)dump.buf, dump.size);
    c4slice_free(dump);
    return result;
}
