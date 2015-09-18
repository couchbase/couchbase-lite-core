//
//  c4Test.cc
//  CBForest
//
//  Created by Jens Alfke on 9/16/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//

#include "c4Test.hh"


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
    return o << std::string((char*)s.buf, s.size) << "\"]";
}


void C4Test::setUp() {
    const char *dbPath = "/tmp/forest_temp.fdb";
    ::unlink(dbPath);
    C4Error error;
    db = c4db_open(c4str(dbPath), false, &error);
    Assert(db != NULL);
}


void C4Test::tearDown() {
    C4Error error;
    if (db)
        Assert(c4db_delete(db, &error));
}


void C4Test::createRev(C4Slice docID, C4Slice revID, C4Slice body) {
    TransactionHelper t(db);
    C4Error error;
    C4Document *doc = c4doc_get(db, docID, false, &error);
    Assert(doc != NULL);
    Assert(c4doc_insertRevision(doc, revID,  body,  false, false, false, &error));
    Assert(c4doc_save(doc, 20, &error));
    c4doc_free(doc);
}


const C4Slice C4Test::kDocID = C4STR("mydoc");
const C4Slice C4Test::kRevID = C4STR("1-abcdef");
const C4Slice C4Test::kBody  = C4STR("{\"name\":007}");


// Dumps a C4Key to a C++ string
std::string toJSON(C4KeyReader r) {
    C4SliceResult dump = c4key_toJSON(&r);
    std::string result((char*)dump.buf, dump.size);
    c4slice_free(dump);
    return result;
}
