//
//  c4Test.hh
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 9/16/15.
//  Copyright (c) 2015-2016 Couchbase. All rights reserved.
//

#pragma once

#include "c4Database.h"
#include "c4Document.h"
#include "c4DocEnumerator.h"
#include "c4View.h"

#include "CatchHelper.hh"
#include "MSVC_Compat.hh"


#ifdef _MSC_VER
#define kTestDir "C:\\tmp\\"
#else
#define kTestDir "/tmp/"
#endif


// Some operators to make C4Slice work with Catch assertions:
bool operator== (C4Slice s1, C4Slice s2);
std::ostream& operator<< (std::ostream& o, C4Slice s);
std::ostream& operator<< (std::ostream &out, C4Error error);


// Dumps a C4Key to a C++ string
std::string toJSON(C4KeyReader);
static inline std::string toJSON(C4Key* key)    {return toJSON(c4key_read(key));}
static inline std::string toString(C4Slice s)   {return std::string((char*)s.buf, s.size);}


// This helper is necessary because it ends an open transaction if an assertion fails.
// If the transaction isn't ended, the c4db_delete call in tearDown will deadlock.
class TransactionHelper {
    public:
    explicit TransactionHelper(C4Database* db) {
        C4Error error;
        REQUIRE(c4db_beginTransaction(db, &error));
        _db = db;
    }

    ~TransactionHelper() {
        if (_db) {
            C4Error error;
            REQUIRE(c4db_endTransaction(_db, true, &error));
        }
    }

    private:
    C4Database* _db {nullptr};
};


// Handy base class that creates a new empty C4Database in its setUp method,
// and closes & deletes it in tearDown.
class C4Test {
public:
    static const int numberOfOptions = 4;
    
    C4Test(int testOption);
    ~C4Test();

    C4Slice databasePath();

protected:
    C4Database *db;

    const C4StorageEngine storageType() const   {return _storage;}
    bool isSQLite() const                       {return storageType() == kC4SQLiteStorageEngine;}
    bool isForestDB() const                     {return storageType() == kC4ForestDBStorageEngine;}
    C4DocumentVersioning versioning() const     {return _versioning;}
    bool isRevTrees() const                     {return _versioning == kC4RevisionTrees;}
    bool isVersionVectors() const               {return _versioning == kC4VersionVectors;}

    // Creates a new document revision with the given revID as a child of the current rev
    void createRev(C4Slice docID, C4Slice revID, C4Slice body, bool isNew = true);

    // Some handy constants to use
    static const C4Slice kDocID;    // "mydoc"
    C4Slice kRevID;    // "1-abcdef"
    C4Slice kRev2ID;   // "2-d00d3333"
    C4Slice kRev3ID;
    static const C4Slice kBody;     // "{\"name\":007}"

private:
    const C4StorageEngine _storage;
    const C4DocumentVersioning _versioning;
    int objectCount;
};
