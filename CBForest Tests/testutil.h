//
//  testutil.h
//  CBForest
//
//  Created by Jens Alfke on 5/13/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#import "Database.hh"

#define Assert      XCTAssert
#define AssertEq    XCTAssertEqual
#define AssertEqual XCTAssertEqualObjects

void CreateTestDir();
std::string PathForDatabaseNamed(NSString *name);
cbforest::Database::config TestDBConfig();
