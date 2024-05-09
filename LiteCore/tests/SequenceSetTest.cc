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

static void checkEmpty(const SequenceSet& s) {
    CHECK(s.empty());
    CHECK(s.size() == 0);
    CHECK(s.first() == 0_seq);
    CHECK(s.last() == 0_seq);
    CHECK(!s.contains(1234_seq));
    CHECK(s.begin() == s.end());
    CHECK(s.to_string() == "[]");
}

TEST_CASE("SequenceSet: empty", "[SequenceSet]") {
    SequenceSet s;
    checkEmpty(s);
    CHECK(!s.remove(1234_seq));
}

TEST_CASE("SequenceSet: single item", "[SequenceSet]") {
    SequenceSet s;
    s.add(1234_seq);

    CHECK(!s.empty());
    CHECK(s.size() == 1);
    CHECK(s.first() == 1234_seq);
    CHECK(s.last() == 1234_seq);
    CHECK(s.contains(1234_seq));

    auto i = s.begin();
    REQUIRE(i != s.end());
    CHECK(i->first == 1234_seq);
    CHECK(i->second == 1235_seq);
    ++i;
    CHECK(i == s.end());
    CHECK(s.to_string() == "[1234]");

    s.add(1234_seq);
    CHECK(s.to_string() == "[1234]");

    CHECK(s.remove(1234_seq));
    checkEmpty(s);
}

TEST_CASE("SequenceSet: two separate items", "[SequenceSet]") {
    SequenceSet s;
    SECTION("Forwards") {
        s.add(100_seq);
        s.add(110_seq);
    }
    SECTION("Reverse") {
        s.add(110_seq);
        s.add(100_seq);
    }

    CHECK(!s.empty());
    CHECK(s.size() == 2);
    CHECK(s.first() == 100_seq);
    CHECK(s.last() == 110_seq);
    CHECK(!s.contains(99_seq));
    CHECK(s.contains(100_seq));
    CHECK(!s.contains(109_seq));
    CHECK(s.contains(110_seq));
    CHECK(!s.contains(111_seq));

    auto i = s.begin();
    REQUIRE(i != s.end());
    CHECK(i->first == 100_seq);
    CHECK(i->second == 101_seq);
    ++i;
    REQUIRE(i != s.end());
    CHECK(i->first == 110_seq);
    CHECK(i->second == 111_seq);
    ++i;
    CHECK(i == s.end());
    CHECK(s.to_string() == "[100, 110]");

    s.add(100_seq);
    CHECK(s.to_string() == "[100, 110]");
    s.add(110_seq);
    CHECK(s.to_string() == "[100, 110]");
}

TEST_CASE("SequenceSet: two consecutive items", "[SequenceSet]") {
    SequenceSet s;
    SECTION("Forwards") {
        s.add(100_seq);
        s.add(101_seq);
    }
    SECTION("Reverse") {
        s.add(101_seq);
        s.add(100_seq);
    }

    CHECK(!s.empty());
    CHECK(s.size() == 2);
    CHECK(s.first() == 100_seq);
    CHECK(s.last() == 101_seq);
    CHECK(!s.contains(99_seq));
    CHECK(s.contains(100_seq));
    CHECK(s.contains(101_seq));
    CHECK(!s.contains(102_seq));

    auto i = s.begin();
    REQUIRE(i != s.end());
    CHECK(i->first == 100_seq);
    CHECK(i->second == 102_seq);
    ++i;
    CHECK(i == s.end());
    CHECK(s.to_string() == "[100-101]");
}

TEST_CASE("SequenceSet: remove item", "[SequenceSet]") {
    SequenceSet s;
    s.add(100_seq);
    s.add(101_seq);

    C4SequenceNumber other = 0_seq;
    SECTION("Remove first") {
        CHECK(s.remove(100_seq));
        other = 101_seq;
        CHECK(s.to_string() == "[101]");
    }
    SECTION("Remove last") {
        CHECK(s.remove(101_seq));
        other = 100_seq;
        CHECK(s.to_string() == "[100]");
    }
    CHECK(!s.empty());
    CHECK(s.size() == 1);

    CHECK(s.remove(other));
    checkEmpty(s);
}

TEST_CASE("SequenceSet: merge ranges", "[SequenceSet]") {
    SequenceSet s;
    s.add(100_seq);
    s.add(101_seq);
    s.add(103_seq);
    s.add(104_seq);
    CHECK(s.to_string() == "[100-101, 103-104]");
    s.add(102_seq);
    CHECK(s.to_string() == "[100-104]");
}

TEST_CASE("SequenceSet: remove", "[SequenceSet]") {
    SequenceSet s;
    s.add(100_seq);
    s.add(101_seq);
    s.add(102_seq);
    s.add(103_seq);
    s.add(104_seq);
    CHECK(s.to_string() == "[100-104]");

    SECTION("Remove 99") { CHECK(!s.remove(99_seq)); }
    SECTION("Remove 105") { CHECK(!s.remove(105_seq)); }
    SECTION("Actually remove") {
        SECTION("Remove 100") {
            CHECK(s.remove(100_seq));
            CHECK(s.to_string() == "[101-104]");
        }
        SECTION("Remove 101") {
            CHECK(s.remove(101_seq));
            CHECK(s.to_string() == "[100, 102-104]");
        }
        SECTION("Remove 102") {
            CHECK(s.remove(102_seq));
            CHECK(s.to_string() == "[100-101, 103-104]");
        }
        SECTION("Remove 103") {
            CHECK(s.remove(103_seq));
            CHECK(s.to_string() == "[100-102, 104]");
        }
        SECTION("Remove 104") {
            CHECK(s.remove(104_seq));
            CHECK(s.to_string() == "[100-103]");
        }
        CHECK(!s.empty());
        CHECK(s.size() == 4);
    }
}

TEST_CASE("SequenceSet: add ranges", "[SequenceSet]") {
    SequenceSet s;

    s.add(100_seq, 100_seq);
    REQUIRE(s.empty());

    s.add(100_seq, 101_seq);
    REQUIRE(s.to_string() == "[100]");

    s.add(200_seq, 210_seq);
    REQUIRE(s.to_string() == "[100, 200-209]");

    SECTION("Extend") {
        s.add(90_seq, 150_seq);
        REQUIRE(s.to_string() == "[90-149, 200-209]");
    }
    SECTION("Merge") {
        s.add(101_seq, 205_seq);
        REQUIRE(s.to_string() == "[100-209]");
    }
    SECTION("Merge multiple") {
        s.add(150_seq, 160_seq);
        s.add(170_seq, 180_seq);
        s.add(300_seq, 400_seq);

        s.add(101_seq, 205_seq);
        REQUIRE(s.to_string() == "[100-209, 300-399]");
    }
}

TEST_CASE("SequenceSet: stress test", "[SequenceSet]") {
    using seq                 = SequenceSet::sequence;
    static constexpr size_t N = 200;

    // Fill 'order' with the sequences [0..kCount), then shuffle them:
    seq order[N];
    for ( size_t i = 0; i < N; ++i ) order[i] = seq(i);
    for ( size_t i = N - 1; i > 0; --i ) {
        auto n = RandomNumber(uint32_t(i));
        swap(order[i], order[n]);
    }

    // Now add sequences in shuffled order:
    SequenceSet s;
    for ( size_t i = 0; i < N; i++ ) {
        s.add(order[i]);
        CHECK(s.size() == i + 1);
        for ( size_t j = 0; j < N; j++ ) CHECK(s.contains(order[j]) == (j <= i));
        //Log("%d: %s", i, dump(s).c_str());
    }

    auto iter = s.begin();
    REQUIRE(iter != s.end());
    CHECK(iter->first == 0_seq);
    CHECK(iter->second == seq(N));
    CHECK(next(iter) == s.end());

    // Remove them in shuffled order:
    for ( size_t i = 0; i < N; i++ ) {
        CHECK(s.size() == N - i);
        for ( size_t j = 0; j < N; j++ ) CHECK(s.contains(order[j]) == (j >= i));
        s.remove(order[i]);
    }

    checkEmpty(s);
}

TEST_CASE("Sequence set merging", "[SequenceSet]") {
    litecore::SequenceSet    s1, s2;
    std::function<bool(int)> expectation;
    SECTION("Equal sets") {
        s1.add(1_seq, 12_seq);
        s2.add(1_seq, 12_seq);
        expectation = ([](int i) { return true; });
    }

    SECTION("Non equal sets") {
        s1.add(1_seq, 6_seq);
        s1.add(7_seq);
        s1.add(9_seq);
        s1.add(11_seq);

        s2.add(1_seq, 11_seq);
        expectation = ([](int i) { return !(i == 6 || i == 8 || i == 10 || i == 11); });
    }

    SECTION("Non equal sets reversed") {
        s2.add(1_seq, 6_seq);
        s2.add(7_seq);
        s2.add(9_seq);
        s2.add(11_seq);

        s1.add(1_seq, 11_seq);
        expectation = ([](int i) { return !(i == 6 || i == 8 || i == 10 || i == 11); });
    }

    SECTION("Alternating") {
        s1.add(1_seq);
        s1.add(3_seq);
        s1.add(5_seq);

        s2.add(2_seq);
        s2.add(4_seq);
        s2.add(6_seq);
        expectation = ([](int i) { return false; });
    }

    litecore::SequenceSet intersection = litecore::SequenceSet::intersection(s1, s2);
    for ( auto i = 1; i < 11; i++ ) { CHECK(intersection.contains(C4SequenceNumber(i)) == expectation(i)); }
}
