//
//  c4DocumentTest.cc
//  LiteCore
//
//  Created by Jens Alfke on 10/24/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#include "c4Test.hh"
#include "c4Private.h"


N_WAY_TEST_CASE_METHOD(C4Test, "FleeceDocs", "[Document][Fleece][C]") {
    importJSONLines("C/tests/names_100.json");
}
