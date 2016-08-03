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
#define AssertNull(ACTUAL)            CPPUNIT_ASSERT_EQUAL((ACTUAL), (__typeof(ACTUAL))nullptr)
#define AssertionFailed(MESSAGE)      CPPUNIT_FAIL(MESSAGE)


#define AssertEqualCStrings(STR1, STR2) Assert(strcmp((STR1),(STR2)) == 0) /*, "Expected \"%s\", got \"%s\"", (STR2), (STR1))*/

#define AssertEqualWithAccuracy(A, B, EPSILON)  Assert(fabs((A)-(B)) <= (EPSILON))


#endif /* CppTest_hh */
