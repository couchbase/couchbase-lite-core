//
//  c4Test.cc
//  CBForest
//
//  Created by Jens Alfke on 9/16/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//

#include "c4Test.hh"
#ifndef _MSC_VER
#include <unistd.h>
#endif

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


void C4Test::setUp() {
    c4_shutdown(NULL);
    
    objectCount = c4_getObjectCount();
    c4log_register(kC4LogWarning, log);
#ifdef _MSC_VER
    const char *dbPath = "C:\\tmp\\forest_temp.fdb";
    ::unlink("C:\\tmp\\forest_temp.fdb");
    ::unlink("C:\\tmp\\forest_temp.fdb.0");
    ::unlink("C:\\tmp\\forest_temp.fdb.1");
    ::unlink("C:\\tmp\\forest_temp.fdb.meta");
#else
    const char *dbPath = "/tmp/forest_temp.fdb";
    ::unlink("/tmp/forest_temp.fdb");
    ::unlink("/tmp/forest_temp.fdb.0");
    ::unlink("/tmp/forest_temp.fdb.1");
    ::unlink("/tmp/forest_temp.fdb.meta");
#endif
    
    C4Error error;
    db = c4db_open(c4str(dbPath), kC4DB_Create, encryptionKey(), &error);
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
    C4Document *doc = c4doc_get(db, docID, false, &error);
    Assert(doc != NULL);
    bool deleted = (body.buf == NULL);
    AssertEqual(c4doc_insertRevision(doc, revID,  body,  deleted, false, false, &error), (int)isNew);
    Assert(c4doc_save(doc, 20, &error));
    c4doc_free(doc);
}


const C4Slice C4Test::kDocID = C4STR("mydoc");
const C4Slice C4Test::kRevID = C4STR("1-abcdef");
const C4Slice C4Test::kRev2ID= C4STR("2-d00d3333");
const C4Slice C4Test::kBody  = C4STR("{\"name\":007}");


// Dumps a C4Key to a C++ string
std::string toJSON(C4KeyReader r) {
    C4SliceResult dump = c4key_toJSON(&r);
    std::string result((char*)dump.buf, dump.size);
    c4slice_free(dump);
    return result;
}
