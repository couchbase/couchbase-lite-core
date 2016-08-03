//
//  CBForestTest.hh
//  CBForest
//
//  Created by Jens Alfke on 5/24/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#ifndef CBForestTest_h
#define CBForestTest_h

#include "slice.hh"

using namespace fleece;


#ifdef _MSC_VER
#define kTestDir "C:\\tmp\\"
#else
#define kTestDir "/tmp/"
#endif

void Log(const char *format, ...) __printflike(1, 2);

std::string stringWithFormat(const char *format, ...);

std::string sliceToHex(slice);
std::string sliceToHexDump(slice, size_t width = 16);

void randomBytes(slice dst);

// Some operators to make slice work with AssertEqual:
// (This has to be declared before including cppunit, because C++ sucks)
std::ostream& operator<< (std::ostream& o, slice s);


#include "CppTest.hh"

#include "Database.hh"

using namespace cbforest;
using namespace std;


class DatabaseTestFixture : public CppUnit::TestFixture {
public:

    Database *db {nullptr};
    KeyStore *store {nullptr};

    Database* newDatabase(std::string path, Database::Options* =nullptr);
    void reopenDatabase(Database::Options *newOptions =nullptr);

    virtual void setUp() override;

    virtual void tearDown() override;

};

#endif /* CBForestTest_h */
