//
// SequenceSetTest.cc
//
// Copyright 2019-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "LiteCoreTest.hh"
#include "SequenceSet.hh"
#include "SecureRandomize.hh"
#include <sstream>

using namespace std;
using namespace litecore;

static void checkEmpty(const SequenceSet &s) {
    CHECK(s.empty());
    CHECK(s.size() == 0);
    CHECK(s.first() == 0);
    CHECK(s.last() == 0);
    CHECK(!s.contains(1234));
    CHECK(s.begin() == s.end());
    CHECK(s.to_string() == "{}");
}

TEST_CASE("SequenceSet: empty", "[SequenceSet]") {
    SequenceSet s;
    checkEmpty(s);
    CHECK(!s.remove(1234));
}

TEST_CASE("SequenceSet: single item", "[SequenceSet]") {
    SequenceSet s;
    s.add(1234);

    CHECK(!s.empty());
    CHECK(s.size() == 1);
    CHECK(s.first() == 1234);
    CHECK(s.last() == 1234);
    CHECK(s.contains(1234));

    auto i = s.begin();
    REQUIRE(i != s.end());
    CHECK(i->first == 1234);
    CHECK(i->second == 1235);
    ++i;
    CHECK(i == s.end());
    CHECK(s.to_string() == "{1234}");

    s.add(1234);
    CHECK(s.to_string() == "{1234}");

    CHECK(s.remove(1234));
    checkEmpty(s);
}

TEST_CASE("SequenceSet: two separate items", "[SequenceSet]") {
    SequenceSet s;
    SECTION("Forwards") {
        s.add(100);
        s.add(110);
    }
    SECTION("Reverse") {
        s.add(110);
        s.add(100);
    }

    CHECK(!s.empty());
    CHECK(s.size() == 2);
    CHECK(s.first() == 100);
    CHECK(s.last() == 110);
    CHECK(!s.contains(99));
    CHECK(s.contains(100));
    CHECK(!s.contains(109));
    CHECK(s.contains(110));
    CHECK(!s.contains(111));

    auto i = s.begin();
    REQUIRE(i != s.end());
    CHECK(i->first == 100);
    CHECK(i->second == 101);
    ++i;
    REQUIRE(i != s.end());
    CHECK(i->first == 110);
    CHECK(i->second == 111);
    ++i;
    CHECK(i == s.end());
    CHECK(s.to_string() == "{100, 110}");

    s.add(100);
    CHECK(s.to_string() == "{100, 110}");
    s.add(110);
    CHECK(s.to_string() == "{100, 110}");
}

TEST_CASE("SequenceSet: two consecutive items", "[SequenceSet]") {
    SequenceSet s;
    SECTION("Forwards") {
        s.add(100);
        s.add(101);
    }
    SECTION("Reverse") {
        s.add(101);
        s.add(100);
    }

    CHECK(!s.empty());
    CHECK(s.size() == 2);
    CHECK(s.first() == 100);
    CHECK(s.last() == 101);
    CHECK(!s.contains(99));
    CHECK(s.contains(100));
    CHECK(s.contains(101));
    CHECK(!s.contains(102));

    auto i = s.begin();
    REQUIRE(i != s.end());
    CHECK(i->first == 100);
    CHECK(i->second == 102);
    ++i;
    CHECK(i == s.end());
    CHECK(s.to_string() == "{100-101}");
}

TEST_CASE("SequenceSet: remove item", "[SequenceSet]") {
    SequenceSet s;
    s.add(100);
    s.add(101);

    int other = 0;
    SECTION("Remove first") {
        CHECK(s.remove(100));
        other = 101;
        CHECK(s.to_string() == "{101}");
    }
    SECTION("Remove last") {
        CHECK(s.remove(101));
        other = 100;
        CHECK(s.to_string() == "{100}");
    }
    CHECK(!s.empty());
    CHECK(s.size() == 1);

    CHECK(s.remove(other));
    checkEmpty(s);
}

TEST_CASE("SequenceSet: merge ranges", "[SequenceSet]") {
    SequenceSet s;
    s.add(100);
    s.add(101);
    s.add(103);
    s.add(104);
    CHECK(s.to_string() == "{100-101, 103-104}");
    s.add(102);
    CHECK(s.to_string() == "{100-104}");
}

TEST_CASE("SequenceSet: remove", "[SequenceSet]") {
    SequenceSet s;
    s.add(100);
    s.add(101);
    s.add(102);
    s.add(103);
    s.add(104);
    CHECK(s.to_string() == "{100-104}");

    SECTION("Remove 99") { CHECK(!s.remove(99)); }
    SECTION("Remove 105") { CHECK(!s.remove(105)); }
    SECTION("Actually remove") {
        SECTION("Remove 100") {
            CHECK(s.remove(100));
            CHECK(s.to_string() == "{101-104}");
        }
        SECTION("Remove 101") {
            CHECK(s.remove(101));
            CHECK(s.to_string() == "{100, 102-104}");
        }
        SECTION("Remove 102") {
            CHECK(s.remove(102));
            CHECK(s.to_string() == "{100-101, 103-104}");
        }
        SECTION("Remove 103") {
            CHECK(s.remove(103));
            CHECK(s.to_string() == "{100-102, 104}");
        }
        SECTION("Remove 104") {
            CHECK(s.remove(104));
            CHECK(s.to_string() == "{100-103}");
        }
        CHECK(!s.empty());
        CHECK(s.size() == 4);
    }
}

TEST_CASE("SequenceSet: add ranges", "[SequenceSet]") {
    SequenceSet s;

    s.add(100, 100);
    REQUIRE(s.empty());

    s.add(100, 101);
    REQUIRE(s.to_string() == "{100}");

    s.add(200, 210);
    REQUIRE(s.to_string() == "{100, 200-209}");

    SECTION("Extend") {
        s.add(90, 150);
        REQUIRE(s.to_string() == "{90-149, 200-209}");
    }
    SECTION("Merge") {
        s.add(101, 205);
        REQUIRE(s.to_string() == "{100-209}");
    }
    SECTION("Merge multiple") {
        s.add(150, 160);
        s.add(170, 180);
        s.add(300, 400);

        s.add(101, 205);
        REQUIRE(s.to_string() == "{100-209, 300-399}");
    }
}

TEST_CASE("SequenceSet: stress test", "[SequenceSet]") {
    static constexpr size_t N = 200;
    using seq                 = SequenceSet::sequence;

    // Fill 'order' with the sequences [0..kCount), then shuffle them:
    seq order[N];
    for ( seq i = 0; i < N; ++i ) order[i] = i;
    for ( seq i = N - 1; i > 0; --i ) {
        auto n = RandomNumber(uint32_t(i));
        swap(order[i], order[n]);
    }

    // Now add sequences in shuffled order:
    SequenceSet s;
    for ( seq i = 0; i < N; i++ ) {
        s.add(order[i]);
        CHECK(s.size() == i + 1);
        for ( seq j = 0; j < N; j++ ) CHECK(s.contains(order[j]) == (j <= i));
        //Log("%d: %s", i, dump(s).c_str());
    }

    auto iter = s.begin();
    REQUIRE(iter != s.end());
    CHECK(iter->first == 0);
    CHECK(iter->second == N);
    CHECK(next(iter) == s.end());

    // Remove them in shuffled order:
    for ( seq i = 0; i < N; i++ ) {
        CHECK(s.size() == N - i);
        for ( seq j = 0; j < N; j++ ) CHECK(s.contains(order[j]) == (j >= i));
        s.remove(order[i]);
    }

    checkEmpty(s);
}
