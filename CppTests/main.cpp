//
//  main.cpp
//  CppTests
//
//  Created by Jens Alfke on 9/14/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//

#include <cppunit/extensions/TestFactoryRegistry.h>
#include <cppunit/ui/text/TextTestRunner.h>

using namespace CppUnit;

int main( int argc, char **argv) {
    TextTestRunner runner;
    TestFactoryRegistry &registry = TestFactoryRegistry::getRegistry();
    runner.addTest( registry.makeTest() );
    return runner.run( "", false ) ? 0 : -1;
}
