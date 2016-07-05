//
//  c4Test.hh
//  CBForest
//
//  Created by Jens Alfke on 9/16/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//

#ifndef c4Test_hh
#define c4Test_hh

#include "c4Database.h"
#include "c4Document.h"
#include "c4DocEnumerator.h"
#include "c4View.h"
#include <cppunit/TestCase.h>
#include <cppunit/extensions/HelperMacros.h>
#include "iostream"


// Less-obnoxious names for assertions:
#define Assert CPPUNIT_ASSERT
#define AssertEqual(ACTUAL, EXPECTED) CPPUNIT_ASSERT_EQUAL(EXPECTED, ACTUAL)


// Some operators to make C4Slice work with AssertEqual:
bool operator== (C4Slice s1, C4Slice s2);
std::ostream& operator<< (std::ostream& o, C4Slice s);

std::string toJSON(C4KeyReader r);



// This helper is necessary because it ends an open transaction if an assertion fails.
// If the transaction isn't ended, the c4db_delete call in tearDown will deadlock.
class TransactionHelper {
    public:
    TransactionHelper(C4Database* db)
    :_db(NULL)
    {
        C4Error error;
        Assert(c4db_beginTransaction(db, &error));
        _db = db;
    }

    ~TransactionHelper() {
        if (_db) {
            C4Error error;
            Assert(c4db_endTransaction(_db, true, &error));
        }
    }

    private:
    C4Database* _db;
};


// Handy base class that creates a new empty C4Database in its setUp method,
// and closes & deletes it in tearDown.
class C4Test : public CppUnit::TestFixture {
public:
    virtual void setUp();
    virtual void tearDown();

protected:
    C4Database *db;

    virtual const C4EncryptionKey* encryptionKey()  {return NULL;}

    // Creates a new document revision
    void createRev(C4Slice docID, C4Slice revID, C4Slice body, bool isNew = true);

    // Some handy constants to use
    static const C4Slice kDocID;    // "mydoc"
    static const C4Slice kRevID;    // "1-abcdef"
    static const C4Slice kRev2ID;   // "2-d00d3333"
    static const C4Slice kBody;     // "{\"name\":007}"

private:
    int objectCount;
};


// Dumps a C4Key to a C++ string
std::string toJSON(C4KeyReader);

static inline std::string toJSON(C4Key* key)    {return toJSON(c4key_read(key));}

static inline std::string toString(C4Slice s)   {return std::string((char*)s.buf, s.size);}

#endif /* c4Test_hh */
