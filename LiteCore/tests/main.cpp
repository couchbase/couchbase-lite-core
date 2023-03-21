//
// main.cpp
//
// Copyright 2015-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

// This defines the main entry point of a target that runs 'Catch' unit tests.


#define CATCH_CONFIG_CONSOLE_WIDTH 120
#define CATCH_CONFIG_MAIN  // This tells Catch to provide a main() - only do this in one cpp file

#include "QuietReporter.hh"  // Includes catch.hpp
#include "catch.hpp"
