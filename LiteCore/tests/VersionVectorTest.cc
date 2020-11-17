//
// VersionVectorTest.cc
//
// Copyright © 2020 Couchbase. All rights reserved.
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

#include "VersionVector.hh"
#include "LiteCoreTest.hh"

using namespace litecore;
using namespace std;

static constexpr peerID Alice {0x100}, Bob {0x101}, Carol {0x102}, Dave {0x103}, Zegpold {0xFFFF};

TEST_CASE("Version", "[VersionVector]") {
    Version v1(1, Alice), v2(1, Alice), v3(2, Alice), v4(1, Bob);
    CHECK(v1.gen() == 1);
    CHECK(v1.author() == Alice);
    CHECK(v1 == v2);
    CHECK(!(v1 == v3));
    CHECK(!(v1 == v4));
    CHECK(v1.asString() == "1@100");
    CHECK(Version("1@100") == v1);
    CHECK(Version("1234@cafebabe") == Version(0x1234, peerID{0xcafebabe}));
    CHECK(Version::compareGen(2, 1) == kNewer);
    CHECK(Version::compareGen(2, 2) == kSame);
    CHECK(Version::compareGen(2, 3) == kOlder);

    Version me(0x3e, kMePeerID);
    CHECK(me.asString() == "3e@*");
    CHECK(Version("3e@*") == me);
}


TEST_CASE("Empty VersionVector", "[VersionVector]") {
    VersionVector v;
    CHECK(!v);
    CHECK(v.count() == 0);
    CHECK(v.versions().size() == 0);
    CHECK(v.isExpanded());
    v.compactMyPeerID(Alice);
    v.expandMyPeerID(Alice);
    CHECK(v.asData().size == 0);
    CHECK(v.compareTo(v) == kSame);
}


TEST_CASE("VersionVector <-> String", "[VersionVector]") {
    VersionVector v("3@*"s);
    CHECK(v.count() == 1);
    CHECK(v[0] == Version(3, kMePeerID));
    CHECK(v.asString() == "3@*");
    CHECK(v.exportAsString(Bob) == "3@101");

    v.parse("3@*,2@100,1@103,2@102");
    CHECK(v.count() == 4);
    CHECK(v[0] == Version(3, kMePeerID));
    CHECK(v[1] == Version(2, Alice));
    CHECK(v[2] == Version(1, Dave));
    CHECK(v[3] == Version(2, Carol));
    CHECK(v.asString() == "3@*,2@100,1@103,2@102");
    CHECK(v.exportAsString(Bob) == "3@101,2@100,1@103,2@102");
}


TEST_CASE("VersionVector <-> Binary", "[VersionVector]") {
    static constexpr uint8_t kBytes[] = {0x03, 0x00,  0x02, 0x80, 0x02,  0x01, 0x83, 0x02,  0x02, 0x82, 0x02};
    static constexpr slice kBinary(kBytes, sizeof(kBytes));
    VersionVector v(kBinary);
    CHECK(v.count() == 4);
    CHECK(v.current() == Version(3, kMePeerID));
    CHECK(v[0] == Version(3, kMePeerID));
    CHECK(v[1] == Version(2, Alice));
    CHECK(v[2] == Version(1, Dave));
    CHECK(v[3] == Version(2, Carol));
    CHECK(v.asString() == "3@*,2@100,1@103,2@102");
    CHECK(v.asData() == kBinary);
}


TEST_CASE("VersionVector peers", "[VersionVector]") {
    VersionVector v("3@*,2@100,1@103,2@102"s);
    CHECK(v.current() == Version(3, kMePeerID));
    CHECK(v.genOfAuthor(Alice) == 2);
    CHECK(v[Alice] == 2);
    CHECK(v[kMePeerID] == 3);
    CHECK(v[Zegpold] == 0);

    CHECK(v.isExpanded() == false);
    v.expandMyPeerID(Bob);
    CHECK(v.isExpanded() == true);
    CHECK(v.asString() == "3@101,2@100,1@103,2@102");

    v.incrementGen(Bob);
    CHECK(v.asString() == "4@101,2@100,1@103,2@102");
    v.incrementGen(Dave);
    CHECK(v.asString() == "2@103,4@101,2@100,2@102");
    v.incrementGen(Zegpold);
    CHECK(v.asString() == "1@ffff,2@103,4@101,2@100,2@102");
}


TEST_CASE("VersionVector conflicts", "[VersionVector]") {
    VersionVector v1("3@*,2@100,1@103,2@102"s);
    CHECK(v1 == v1);
    CHECK(v1 == VersionVector("3@*,2@100,1@103,2@102"s));

    CHECK(v1 > VersionVector("2@*,2@100,1@103,2@102"s));
    CHECK(v1 > VersionVector("2@100,1@103,2@102"s));
    CHECK(v1 > VersionVector("1@102"s));
    CHECK(v1 > VersionVector());

    CHECK(v1 < VersionVector("2@103,3@*,2@100,2@102"s));
    CHECK(v1 < VersionVector("2@103,1@666,3@*,2@100,9@102"s));

    VersionVector v3("4@100,1@103,2@102"s);
    CHECK(v1.compareTo(v3) == kConflicting);
    CHECK(!(v1 == v3));
    CHECK(!(v1 < v3));
    CHECK(!(v1 > v3));

    CHECK(v1.mergedWith(v3).asString() == "3@*,4@100,1@103,2@102");
}
