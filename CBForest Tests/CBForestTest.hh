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

std::string sliceToHex(slice);
std::string sliceToHexDump(slice, size_t width = 16);

// Some operators to make slice work with AssertEqual:
// (This has to be declared before including cppunit, because C++ sucks)
std::ostream& operator<< (std::ostream& o, slice s);


#include "CppTest.hh"

#include "Database.hh"

using namespace cbforest;


class DatabaseTestFixture : public CppUnit::TestFixture {
public:

    Database *db {NULL};

    virtual void setUp();

    virtual void tearDown();

};

#endif /* CBForestTest_h */
