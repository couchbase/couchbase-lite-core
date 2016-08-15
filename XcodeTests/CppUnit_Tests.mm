//
//  CppUnit_Tests.m
//  CBForest
//
//  Created by Jens Alfke on 8/12/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#import <XCTest/XCTest.h>
#include <cppunit/extensions/TestFactoryRegistry.h>
#include <cppunit/ui/text/TextTestRunner.h>

using namespace CppUnit;


@interface CppUnit_Tests : XCTestCase
@end

@implementation CppUnit_Tests

- (void)testCppUnit {
    NSLog(@"Starting CppUnit tests...");
    TextTestRunner runner;
    TestFactoryRegistry &registry = TestFactoryRegistry::getRegistry();
    runner.addTest( registry.makeTest() );
    XCTAssert(runner.run( "", false ));
}

@end
