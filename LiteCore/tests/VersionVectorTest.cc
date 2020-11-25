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
#include "RevTree.hh"
#include "LiteCoreTest.hh"

using namespace litecore;
using namespace std;

namespace litecore {
    // Some implementations of "<<" to write to ostreams:

    /** Writes an ASCII representation of a Version to a stream.
     Note: This does not replace "*" with the local author's ID! */
    static inline std::ostream& operator<< (std::ostream& o, const Version &v) {
        return o << v.asASCII();
    }

    /** Writes an ASCII representation of a VersionVector to a stream.
     Note: This does not replace "*" with the local author's ID! */
    static inline std::ostream& operator<< (std::ostream& o, const VersionVector &vv) {
        return o << vv.asASCII();
    }
}

static constexpr peerID Alice {0x100}, Bob {0x101}, Carol {0x102}, Dave {0x103}, Zegpold {0xFFFF};


TEST_CASE("Version", "[RevIDs]") {
    Version v1(1, Alice), v2(1, Alice), v3(2, Alice), v4(1, Bob);
    CHECK(v1.gen() == 1);
    CHECK(v1.author() == Alice);
    CHECK(v1 == v2);
    CHECK(!(v1 == v3));
    CHECK(!(v1 == v4));
    CHECK(v1.asASCII() == "1@100"_sl);
    CHECK(Version("1@100") == v1);
    CHECK(Version("1234@cafebabe") == Version(0x1234, peerID{0xcafebabe}));
    CHECK(Version::compareGen(2, 1) == kNewer);
    CHECK(Version::compareGen(2, 2) == kSame);
    CHECK(Version::compareGen(2, 3) == kOlder);

    Version me(0x3e, kMePeerID);
    CHECK(me.asASCII() == "3e@*"_sl);
    CHECK(Version("3e@*") == me);
}


TEST_CASE("Empty VersionVector", "[RevIDs]") {
    VersionVector v;
    CHECK(!v);
    CHECK(v.count() == 0);
    CHECK(v.versions().size() == 0);
    CHECK(v.asASCII() == ""_sl);
    CHECK(v.isExpanded());
    CHECK(v.asBinary().size == 1);
    v.compactMyPeerID(Alice);
    v.expandMyPeerID(Alice);
    CHECK(v.compareTo(v) == kSame);
}


TEST_CASE("VersionVector <-> String", "[RevIDs]") {
    auto v = VersionVector::fromASCII("3@*"s);
    CHECK(v.count() == 1);
    CHECK(v[0] == Version(3, kMePeerID));
    CHECK(v.asASCII() == "3@*");
    CHECK(v.asASCII(Bob) == "3@101");

    v.readASCII("3@*,2@100,1@103,2@102");
    CHECK(v.count() == 4);
    CHECK(v[0] == Version(3, kMePeerID));
    CHECK(v[1] == Version(2, Alice));
    CHECK(v[2] == Version(1, Dave));
    CHECK(v[3] == Version(2, Carol));
    CHECK(v.asASCII() == "3@*,2@100,1@103,2@102");
    CHECK(v.asASCII(Bob) == "3@101,2@100,1@103,2@102");
}


TEST_CASE("VersionVector <-> Binary", "[RevIDs]") {
    static constexpr uint8_t kBytes[] = {0x00,
                                         0x03, 0x00,
                                         0x02, 0x80, 0x02,
                                         0x01, 0x83, 0x02,
                                         0x02, 0x82, 0x02};
    static constexpr slice kBinary(kBytes, sizeof(kBytes));
    VersionVector v;
    v.readBinary(kBinary);
    CHECK(v.count() == 4);
    CHECK(v.current() == Version(3, kMePeerID));
    CHECK(v[0] == Version(3, kMePeerID));
    CHECK(v[1] == Version(2, Alice));
    CHECK(v[2] == Version(1, Dave));
    CHECK(v[3] == Version(2, Carol));
    CHECK(v.asASCII() == "3@*,2@100,1@103,2@102");
    CHECK(v.asBinary() == kBinary);
}


TEST_CASE("VersionVector peers", "[RevIDs]") {
    auto v = VersionVector::fromASCII("3@*,2@100,1@103,2@102"s);
    CHECK(v.current() == Version(3, kMePeerID));
    CHECK(v.genOfAuthor(Alice) == 2);
    CHECK(v[Alice] == 2);
    CHECK(v[kMePeerID] == 3);
    CHECK(v[Zegpold] == 0);

    CHECK(v.isExpanded() == false);
    v.expandMyPeerID(Bob);
    CHECK(v.isExpanded() == true);
    CHECK(v.asASCII() == "3@101,2@100,1@103,2@102");

    v.incrementGen(Bob);
    CHECK(v.asASCII() == "4@101,2@100,1@103,2@102");
    v.incrementGen(Dave);
    CHECK(v.asASCII() == "2@103,4@101,2@100,2@102");
    v.incrementGen(Zegpold);
    CHECK(v.asASCII() == "1@ffff,2@103,4@101,2@100,2@102");
}


TEST_CASE("VersionVector conflicts", "[RevIDs]") {
    auto v1 = VersionVector::fromASCII("3@*,2@100,1@103,2@102"s);
    CHECK(v1 == v1);
    CHECK(v1 == VersionVector::fromASCII("3@*,2@100,1@103,2@102"s));

    CHECK(v1 > VersionVector::fromASCII("2@*,2@100,1@103,2@102"s));
    CHECK(v1 > VersionVector::fromASCII("2@100,1@103,2@102"s));
    CHECK(v1 > VersionVector::fromASCII("1@102"s));
    CHECK(v1 > VersionVector());

    CHECK(v1 < VersionVector::fromASCII("2@103,3@*,2@100,2@102"s));
    CHECK(v1 < VersionVector::fromASCII("2@103,1@666,3@*,2@100,9@102"s));

    auto v3 = VersionVector::fromASCII("4@100,1@103,2@102"s);
    CHECK(v1.compareTo(v3) == kConflicting);
    CHECK(!(v1 == v3));
    CHECK(!(v1 < v3));
    CHECK(!(v1 > v3));

    CHECK(v1.mergedWith(v3).asASCII() == "3@*,4@100,1@103,2@102");
}


#pragma mark - REVID:


struct DigestTestCase {
    const char *str;
    uint64_t gen;
    slice digest;
    const char *hex;
};


TEST_CASE("RevID Parsing", "[RevIDs]") {
    static constexpr DigestTestCase kCases[] = {
        // good:
        {"1-aa",                    1,      "\xaa",                             "01aa"},
        {"97-beef",                 97,     "\xbe\xef",                         "61beef"},
        {"1-1234567890abcdef",      1,      "\x12\x34\x56\x78\x90\xab\xcd\xef", "011234567890abcdef"},
        {"123456-1234567890abcdef", 123456, "\x12\x34\x56\x78\x90\xab\xcd\xef", "c0c4071234567890abcdef"},

        // bad:
        {""},
        {"1"},
        {"1-"},
        {"1-0"},
        {"1-a"},
        {"1-AA"},
        {"1-aF"},
        {"1--aa"},
        {"0-11"},
        {"-1-11"},
        {"-11"},
        {"a-11"},
        {"1-aa "},
        {"z-aa"},
        {"d-aa"},
        {"7-ax"},
        {" 1-aa"},
        {"12345678123456789-aa"},    // gen too large; below is digest too large
        {"1-deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef"},
    };

    for (auto c4se : kCases) {
        INFO("Testing '" << c4se.str << "'");
        revidBuffer r;
        if (c4se.gen) {
            CHECK(r.tryParse(slice(c4se.str)));
            CHECK(!r.isVersion());
            CHECK(r.generation() == c4se.gen);
            CHECK(r.digest() == c4se.digest);
            CHECK(r.expanded() == slice(c4se.str));
            CHECK(r.hexString() == c4se.hex);
        } else {
            CHECK(!r.tryParse(slice(c4se.str)));
        }
    }

}


struct VersionTestCase {
    const char *str;
    uint64_t gen;
    uint64_t peer;
    const char *hex;
};


TEST_CASE("RevID Version Parsing", "[RevIDs]") {
    static constexpr VersionTestCase kCases[] = {
        // good:
        {"1@*",   0x1,   0x0, "000100"},
        {"bff@3", 0xbff, 0x3, "00ff1703"},
        {"c@c",   0xc,   0xc, "000c0c"},
        {"d00d@*",0xd00d,0x0, "008da00300"},
        {"d00d@*",0xd00d,0x0, "008da00300"},
        {"ffffffffffffffff@1", 0xffffffffffffffff, 0x1, "00ffffffffffffffffff0101"},
        {"1@ffffffffffffffff", 0x1, 0xffffffffffffffff, "0001ffffffffffffffffff01"},

        // bad:
        {"0@11"},                   // gen can't be 0
        {"1@0"},                    // peerID can't be literal 0 (must be '*')
        {"12345678123456789@*"},    // gen too large
        {"1@12345678123456789"},    // peerID too large
        {"@"},
        {"*"},
        {"*@*"},
        {"1"},
        {"1@"},
        {"1-@"},
        {"1"},
        {"1@"},
        {"1@*1"},
        {"1@**"},
        {"1@1-"},
        {"1@-1"},
        {"1@@aa"},
        {"@1@11"},
        {"@11"},
        {"z@aa"},
        {"7@ax"},
        {" 1@aa"},
        {"1 @aa"},
        {"1@ aa"},
        {"1@a a"},
        {"1@aa "},
    };

    for (auto c4se : kCases) {
        INFO("Testing '" << c4se.str << "'");
        revidBuffer r;
        if (c4se.gen) {
            CHECK(r.tryParse(slice(c4se.str)));
            CHECK(r.isVersion());
            //CHECK(r.generation() == c4se.gen);
            CHECK(r.asVersion().gen() == c4se.gen);
            CHECK(r.asVersion().author().id == c4se.peer);
            CHECK(r.expanded() == slice(c4se.str));
            CHECK(r.hexString() == c4se.hex);
        } else {
            CHECK(!r.tryParse(slice(c4se.str)));
        }
    }
}


TEST_CASE("RevID <-> Version", "[RevIDs]") {
    auto vv = VersionVector::fromASCII("11@100,2@101,1@666"s);
    alloc_slice vvData = vv.asBinary();
    revid rev(vvData);
    CHECK(rev.isVersion());
    CHECK(rev.asVersion() == Version(17,Alice));
    CHECK(rev.asVersionVector() == vv);
    CHECK(rev.expanded() == "11@100"_sl);           // revid only looks at the current Version

    revidBuffer r(Version(17, Alice));
    CHECK(r.isVersion());
    CHECK(r.asVersion() == Version(17,Alice));
    CHECK(r.expanded() == "11@100"_sl);
}
