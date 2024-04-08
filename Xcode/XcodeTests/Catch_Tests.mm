//
//  Catch_Tests.m
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 8/26/16.
//  Copyright 2016-Present Couchbase, Inc.
//
//  Use of this software is governed by the Business Source License included
//  in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
//  in that file, in accordance with the Business Source License, use of this
//  software will be governed by the Apache License, Version 2.0, included in
//  the file licenses/APL2.txt.
//

#import <XCTest/XCTest.h>

#define CATCH_CONFIG_CONSOLE_WIDTH 120
#define CATCH_CONFIG_RUNNER

#import "c4Test.hh"
#include "catch.hpp"


@interface Catch_Tests : XCTestCase
@end


@implementation Catch_Tests

- (void)testCatchTests {
    C4Test::sFixturesDir = std::string([[NSBundle bundleForClass: [self class]] resourcePath].UTF8String) + "/data/";

    Catch::Session session;
//    session.configData().reporterNames.push_back("list");
    session.configData().useColour = Catch::UseColour::No; // otherwise it tries to use ANSI escapes in Xcode console

    NSArray* args = [NSProcessInfo.processInfo arguments];
    NSUInteger nargs = args.count;
    const char* argv[nargs];
    int argc = 0;
    for (NSUInteger i = 0; i < nargs; i++) {
        const char* arg = [args[i] UTF8String];
        if (i > 0 && arg[0] == '-' && isupper(arg[1])) {    // Ignore Cocoa arguments
            ++i;
            continue;
        }
        argv[argc++] = arg;
    }

    XCTAssertEqual(session.applyCommandLine(argc, argv), 0);
    XCTAssertEqual(session.run(), 0);
}

@end


#include "QuietReporter.hh"
