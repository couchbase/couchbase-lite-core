//
//  CppTest.hh
//  CBForest
//
//  Created by Jens Alfke on 5/24/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#ifndef CppTest_hh
#define CppTest_hh

#include <cppunit/TestCase.h>
#include <cppunit/extensions/HelperMacros.h>


// Less-obnoxious names for assertions:
#define Assert CPPUNIT_ASSERT
#define AssertEqual(ACTUAL, EXPECTED) CPPUNIT_ASSERT_EQUAL(EXPECTED, ACTUAL)


#endif /* CppTest_hh */
