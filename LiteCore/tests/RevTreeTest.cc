//
//  RevTreeTest.cc
//
// Copyright (c) 2018 Couchbase, Inc All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "RevTree.hh"

#include "LiteCoreTest.hh"

using namespace litecore;
using namespace std;


TEST_CASE("RevID Parsing") {
    revidBuffer r;

    CHECK(r.tryParse("1-aa"_sl));
    CHECK(r.generation() == 1);
    CHECK(r.tryParse("1-beef"_sl));
    CHECK(r.generation() == 1);
    CHECK(r.tryParse("1-1234567890abcdef"_sl));
    CHECK(r.generation() == 1);
    CHECK(r.tryParse("123456-1234567890abcdef"_sl));
    CHECK(r.generation() == 123456);

    CHECK(!r.tryParse("1"_sl));
    CHECK(!r.tryParse("1-"_sl));
    CHECK(!r.tryParse("1-0"_sl));
    CHECK(!r.tryParse("1-a"_sl));
    CHECK(!r.tryParse("1-AA"_sl));
    CHECK(!r.tryParse("1-aF"_sl));
    CHECK(!r.tryParse("1--aa"_sl));
    CHECK(!r.tryParse("0-11"_sl));
    CHECK(!r.tryParse("-1-11"_sl));
    CHECK(!r.tryParse("-11"_sl));
    CHECK(!r.tryParse("a-11"_sl));
    CHECK(!r.tryParse("1-aa "_sl));
    CHECK(!r.tryParse(" 1-aa"_sl));
}
