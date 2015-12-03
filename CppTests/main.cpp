//
//  main.cpp
//  CppTests
//
//  Created by Jens Alfke on 9/14/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//

#include <cppunit/extensions/TestFactoryRegistry.h>
#include <cppunit/ui/text/TestRunner.h>


int main( int argc, char **argv) {
    CppUnit::TextUi::TestRunner runner;
    CppUnit::TestFactoryRegistry &registry = CppUnit::TestFactoryRegistry::getRegistry();
    runner.addTest( registry.makeTest() );
    return runner.run( "", false ) ? 0 : -1;
}
