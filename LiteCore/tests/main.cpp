//
//  main.cpp
//  CppTests
//
//  Created by Jens Alfke on 9/14/15.
//  Copyright (c) 2015-2016 Couchbase. All rights reserved.
//

// This defines the main entry point of a target that runs 'Catch' unit tests.


#define CATCH_CONFIG_CONSOLE_WIDTH 120
#define CATCH_CONFIG_MAIN  // This tells Catch to provide a main() - only do this in one cpp file
#include "catch.hpp"

#include "CaseListReporter.hh"
