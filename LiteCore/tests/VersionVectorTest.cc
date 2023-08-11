//
// VersionVectorTest.cc
//
// Copyright 2020-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "VersionVector.hh"
#include "HybridClock.hh"
#include "RevTree.hh"
#include "LiteCoreTest.hh"
#include "StringUtil.hh"
#include "slice_stream.hh"
#include "c4DocumentTypes.h" // for C4RevIDInfo
#include <iomanip>
#include <thread>

using namespace litecore;
using namespace std;

namespace litecore {
    // Some implementations of "<<" to write to ostreams:

    static inline std::ostream& operator<<(std::ostream& o, logicalTime t) {
        return o << std::hex << uint64_t(t) << std::dec;
    }

    /** Writes an ASCII representation of a Version to a stream.
     Note: This does not replace "*" with the local author's ID! */
    static inline std::ostream& operator<<(std::ostream& o, const Version& v) {
        return o << "Version(" << string(v.asASCII()) << ")";
    }

    /** Writes an ASCII representation of a VersionVector to a stream.
     Note: This does not replace "*" with the local author's ID! */
    static inline std::ostream& operator<<(std::ostream& o, const VersionVector& vv) {
        return o << "VersionVector(" << string(vv.asASCII()) << ")";
    }

    static inline std::ostream& operator<<(std::ostream& o, const optional<VersionVector>& vv) {
        if ( vv ) return o << *vv;
        else
            return o << "nullopt";
    }
}  // namespace litecore

// `_ht` suffix after a numeric literal makes it a hybrid time.
static logicalTime operator"" _ht(unsigned long long i) { return logicalTime{i}; }

// `_vv` suffix after a string literal makes it a VersionVector.
static VersionVector operator"" _vv(const char* str NONNULL, size_t length) {
    if ( length == 0 ) return {};
    return VersionVector::fromASCII(slice(str, length));
}

// `_pid` suffix after a base64 string literal makes it a SourceID.
static SourceID operator"" _pid(const char* str NONNULL, size_t length) {
    if ( length == 1 && str[0] == '*' ) return kMeSourceID;
    SourceID id;
    if ( !id.readASCII(slice(str, length)) ) throw std::invalid_argument("invalid SourceID");
    return id;
}

// Some SourceIDs to test with. Any 22-character string in the base64 character set will work,
// as long as the last character is 'A', 'Q', 'g' or 'w' (whose encodings end in 0000.)
static const SourceID Alice   = "AliceAliceAliceAliceAA"_pid;
static const SourceID Bob     = "BobBobBobBobBobBobBobA"_pid;
static const SourceID Carol   = "CarolCarolCarolCarolCA"_pid;
static const SourceID Dave    = "DaveDaveDaveDaveDaveDA"_pid;
static const SourceID Zegpold = "ZegpoldZegpoldZegpoldA"_pid;


#pragma mark - HYBRID CLOCK:

TEST_CASE("Fake HybridClock", "[RevIDs]") {
    HybridClock clock;
    clock.setSource(make_unique<FakeClockSource>());

    CHECK(!clock.validTime(0_ht));

    CHECK(clock.now() == 0x10000_ht);
    auto n = clock.now();
    CHECK(n == 0x20000_ht);
    CHECK(clock.validTime(n));
}

TEST_CASE("RealClockSource", "[RevIDs]") {
    // Sanity check RealClockSource:
    auto wallNow = uint64_t(RealClockSource{}.now());
    CHECK(wallNow > 0x1773b22e5a655ca0);  // 20 July 2023
    CHECK(wallNow < 0x3000000000000000);  // somewhere in 2079
    cout << "RealClockSource time was " << wallNow << std::endl;

    // Make sure the clock source has at least microsecond resolution:
    for ( int attempt = 0; attempt < 10; ++attempt ) {
        if ( wallNow % 1'000'000 != 0 ) break;
        this_thread::sleep_for(123us);
        wallNow = uint64_t(RealClockSource{}.now());
    }
    CHECK(wallNow % 1'000'000 != 0);
}

TEST_CASE("HybridClock", "[RevIDs]") {
    HybridClock c;
    auto        t  = c.now();
    auto        t2 = c.now();
    CHECK(t2 > t);
    cout << "HybridClock time was " << t << ", then " << t2 << std::endl;

    // Receive a fictitious timestamp from a peer that's 5 seconds ahead:
    auto tSeen = logicalTime{uint64_t(t) + 5 * kNsPerSec};
    CHECK(c.see(tSeen));
    CHECK(c.now() > tSeen);

    // Receive a bogus timestamp that's an hour ahead:
    auto tBogus = logicalTime{uint64_t(t) + 3600 * kNsPerSec};
    CHECK_FALSE(c.see(tBogus));

    // Receive a bogus timestamp from before I even implemented HybridClock:
    auto tBogusPast = 0x166c9b7676dd86a8_ht;
    CHECK_FALSE(c.see(tBogusPast));

    auto t3 = c.now();
    CHECK(t3 > tSeen);
    CHECK(t3 < tBogus);

    uint64_t state = c.state();

    double dState(state);
    cout << "Error from double conversion is " << (state - uint64_t(dState)) << "ns" << endl;

    // Reconstitute clock from its state:
    HybridClock c2(state);
    auto        t4 = c2.now();
    CHECK(t4 > t3);
    CHECK(uint64_t(t4) - uint64_t(t3) < 1e9);
}

#pragma mark - PEER ID:

TEST_CASE("SourceID Binary", "[RevIDs]") {
    for ( const uint8_t& b : kMeSourceID.bytes() ) { CHECK(b == 0); }

    uint8_t xb = 0x1e;
    for ( const uint8_t& b : kLegacyRevSourceID.bytes() ) {
        CHECK(b == xb);
        xb = 0;
    }

    SourceID id;
    for ( size_t i = 0; i < sizeof(SourceID); ++i ) id.bytes()[i] = uint8_t(i + 1);
    CHECK(id != kMeSourceID);
    CHECK(id == id);

    alloc_slice buf(100);
    for ( int current = 0; current <= 1; ++current ) {
        SourceID id2;
        bool     isCurrent;
        {
            slice_ostream out(buf);
            REQUIRE(kMeSourceID.writeBinary(out, current));
            slice result = out.output();
            CHECK(result.hexString() == (current ? "80" : "00"));

            slice_istream in(result);
            REQUIRE(id2.readBinary(in, &isCurrent));
            CHECK(in.eof());
            CHECK(id2 == kMeSourceID);
            CHECK(isCurrent == current);
        }
        {
            slice_ostream out(buf);
            REQUIRE(kLegacyRevSourceID.writeBinary(out, current));
            slice result = out.output();
            CHECK(result.hexString() == (current ? "811e" : "011e"));

            slice_istream in(result);
            REQUIRE(id2.readBinary(in, &isCurrent));
            CHECK(in.eof());
            CHECK(id2 == kLegacyRevSourceID);
            CHECK(isCurrent == current);
        }
        {
            slice_ostream out(buf);
            REQUIRE(id.writeBinary(out, current));
            slice result = out.output();
            CHECK(result.hexString()
                  == (current ? "900102030405060708090a0b0c0d0e0f10" : "100102030405060708090a0b0c0d0e0f10"));

            slice_istream in(result);
            REQUIRE(id2.readBinary(in, &isCurrent));
            CHECK(in.eof());
            CHECK(id2 == id);
            CHECK(isCurrent == current);
        }
    }
}

TEST_CASE("SourceID ASCII", "[RevIDs]") {
    REQUIRE(kMeSourceID.asASCII() == "AAAAAAAAAAAAAAAAAAAAAA");
    CHECK("*"_pid == kMeSourceID);
    CHECK("*"_pid.isMe());

    SourceID id;
    CHECK(id == kMeSourceID);
    CHECK(id.isMe());
    CHECK_FALSE(id.readASCII("AAAAAAAAAAAAAAAAAAAAAB"));
    CHECK_FALSE(id.readASCII("AAAAAAAAAAAAAAAAAAAAAC"));
    CHECK_FALSE(id.readASCII("AAAAAAAAAAAAAAAAAAAAAD"));
    CHECK_FALSE(id.readASCII("AAAAAAAAAAAAAAAAAAAAAI"));
    REQUIRE(id.readASCII("AAAAAAAAAAAAAAAAAAAAAQ"));  // 'Q' in base64 is 110000
    CHECK(id != kMeSourceID);
    CHECK(!id.isMe());

    CHECK(id.asASCII() == "AAAAAAAAAAAAAAAAAAAAAQ");
}

TEST_CASE("RevID Info", "[RevIDs]") {
    // Tree-based revID:
    C4RevIDInfo info;
    info = revidBuffer::getRevIDInfo("123-abcdeabcdeabcdeabcdeabcdeabcdeabcdeabcde");
    CHECK(!info.isVersion);
    CHECK(info.tree.generation == 123);
    CHECK(info.tree.digestString == "abcdeabcdeabcdeabcdeabcdeabcdeabcdeabcde"_sl);
    CHECK(slice(info.tree.digest, sizeof(info.tree.digest)).hexString() == "abcdeabcdeabcdeabcdeabcdeabcdeabcdeabcde");

    // Version by me:
    info = revidBuffer::getRevIDInfo("177a6f04d70d0000@*");
    CHECK(info.isVersion);
    CHECK(info.version.timestamp == 0x177a6f04d70d0000);
    CHECK(info.version.sourceString == "*"_sl);
    CHECK( slice(info.version.source, sizeof(info.version.source)).hexString() == "00000000000000000000000000000000");
    CHECK(info.version.clockTime == 1691786676);
    CHECK(info.version.legacyGen == 0);

    // Version by someone else:
    info = revidBuffer::getRevIDInfo("177a6f04d70d0000@ZegpoldZegpoldZegpoldA");
    CHECK(info.isVersion);
    CHECK(info.version.timestamp == 0x177a6f04d70d0000);
    CHECK(info.version.sourceString == "ZegpoldZegpoldZegpoldA"_sl);
    CHECK( slice(info.version.source, sizeof(info.version.source)).hexString() == "65e829a257597a0a6895d65e829a2574");
    CHECK(info.version.legacyGen == 0);

    char timebuf[100];
    strftime(timebuf, 100, "%F %T", gmtime(&info.version.clockTime));
    CHECK(string(timebuf) == "2023-08-11 20:44:36");

    // Legacy version upgraded from tree-based id:
    info = revidBuffer::getRevIDInfo("177000000000007b@?");
    CHECK(info.isVersion);
    CHECK(info.version.timestamp == 0x177000000000007b);
    CHECK(info.version.sourceString == "?"_sl);
    CHECK( slice(info.version.source, sizeof(info.version.source)).hexString() == "1e000000000000000000000000000000");
    CHECK(info.version.clockTime == 0);
    CHECK(info.version.legacyGen == 123);
}

#pragma mark - VERSION VECTOR:

TEST_CASE("Version", "[RevIDs]") {
    CHECK(Version(1_ht, kMeSourceID).asASCII() == "1@*");
    CHECK(Version(2_ht, kLegacyRevSourceID).asASCII() == "2@?");

    Version v1(1_ht, Alice), v2(1_ht, Alice), v3(2_ht, Alice), v4(1_ht, Bob);
    CHECK(v1.time() == 1_ht);
    CHECK(v1.author() == Alice);
    CHECK(v1 == v2);
    CHECK(!(v1 == v3));
    CHECK(!(v1 == v4));
    CHECK(v1.asASCII() == "1@AliceAliceAliceAliceAA"_sl);
    CHECK(Version("1@AliceAliceAliceAliceAA") == v1);
    CHECK(Version("1234@cafebabecafebabecafebA") == Version(0x1234_ht, "cafebabecafebabecafebA"_pid));
    CHECK(Version::compare(2_ht, 1_ht) == kNewer);
    CHECK(Version::compare(2_ht, 2_ht) == kSame);
    CHECK(Version::compare(2_ht, 3_ht) == kOlder);

    Version me(0x3e_ht, kMeSourceID);
    CHECK(me.asASCII() == "3e@*"_sl);
    CHECK(me.asASCII(Alice) == "3e@AliceAliceAliceAliceAA"_sl);
    CHECK(Version("3e@*") == me);
    CHECK(Version("3e@AliceAliceAliceAliceAA", Alice) == me);
}

TEST_CASE("Empty VersionVector", "[RevIDs]") {
    VersionVector v;
    CHECK(!v);
    CHECK(v.count() == 0);
    CHECK(v.versions().empty());
    CHECK(v.asASCII() == ""_sl);
    CHECK(v.asBinary().size == 1);
    CHECK(v.compareTo(v) == kSame);
}

TEST_CASE("VersionVector <-> String", "[RevIDs]") {
    VersionVector v = "3@*"_vv;
    CHECK(v.count() == 1);
    CHECK(v.currentVersions() == 1);
    CHECK(v[0] == Version(3_ht, kMeSourceID));
    CHECK(v.asASCII() == "3@*");
    CHECK(v.asASCII(Bob) == "3@BobBobBobBobBobBobBobA");

    v.readASCII("3@*, 2@AliceAliceAliceAliceAA,  1@DaveDaveDaveDaveDaveDA,2@CarolCarolCarolCarolCA");
    CHECK(v.count() == 4);
    CHECK(v.currentVersions() == 1);
    CHECK(v[0] == Version(3_ht, kMeSourceID));
    CHECK(v[1] == Version(2_ht, Alice));
    CHECK(v[2] == Version(1_ht, Dave));
    CHECK(v[3] == Version(2_ht, Carol));
    CHECK(v.asASCII() == "3@*; 2@AliceAliceAliceAliceAA, 1@DaveDaveDaveDaveDaveDA, 2@CarolCarolCarolCarolCA");
    CHECK(v.asASCII(Bob)
          == "3@BobBobBobBobBobBobBobA; 2@AliceAliceAliceAliceAA, 1@DaveDaveDaveDaveDaveDA, "
             "2@CarolCarolCarolCarolCA");

    // Parse a vector that has the same peer twice, due to conflict resolution:
    v.readASCII("4@BobBobBobBobBobBobBobA, 3@AliceAliceAliceAliceAA, 2@BobBobBobBobBobBobBobA; "
                "1@CarolCarolCarolCarolCA",
                Bob);
    CHECK(v.count() == 4);
    CHECK(v.currentVersions() == 3);
    CHECK(v[0] == Version(4_ht, kMeSourceID));
    CHECK(v[1] == Version(3_ht, Alice));
    CHECK(v[2] == Version(2_ht, kMeSourceID));
    CHECK(v[3] == Version(1_ht, Carol));

    for ( uint8_t b : v.asBinary() ) fprintf(stderr, "0x%02X, ", b);
    fprintf(stderr, "\n");
}

TEST_CASE("VersionVector <-> Binary", "[RevIDs]") {
    static constexpr uint8_t kBytes[] = {
            0x00, 0x07, 0x80, 0x03, 0x90, 0x02, 0x58, 0x9C, 0x78, 0x09, 0x62, 0x71, 0xE0, 0x25, 0x89, 0xC7, 0x80,
            0x96, 0x27, 0x1E, 0x00, 0x03, 0x90, 0x0D, 0xAB, 0xDE, 0x0D, 0xAB, 0xDE, 0x0D, 0xAB, 0xDE, 0x0D, 0xAB,
            0xDE, 0x0D, 0xAB, 0xDE, 0x0C, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x10, 0x09,
            0xAA, 0xE8, 0x94, 0x26, 0xAB, 0xA2, 0x50, 0x9A, 0xAE, 0x89, 0x42, 0x6A, 0xBA, 0x25, 0x08};
    static constexpr slice kBinary(kBytes, sizeof(kBytes));
    VersionVector          v;
    v.readBinary(kBinary);
    CHECK(v.count() == 4);
    CHECK(v.current() == Version(3_ht, kMeSourceID));
    CHECK(v[0] == Version(3_ht, kMeSourceID));
    CHECK(v[1] == Version(2_ht, Alice));
    CHECK(v[2] == Version(1_ht, Dave));
    CHECK(v[3] == Version(2_ht, Carol));
    CHECK(v.asASCII() == "3@*, 2@AliceAliceAliceAliceAA, 1@DaveDaveDaveDaveDaveDA; 2@CarolCarolCarolCarolCA");
    CHECK(v.asBinary() == kBinary);
}

TEST_CASE("VersionVector authors", "[RevIDs]") {
    HybridClock clock;
    clock.setSource(make_unique<FakeClockSource>(0, 0));

    VersionVector v = "4@*; 3@AliceAliceAliceAliceAA, 2@DaveDaveDaveDaveDaveDA, 1@CarolCarolCarolCarolCA"_vv;
    CHECK(v.current() == Version(0x4_ht, kMeSourceID));
    CHECK(v.timeOfAuthor(Alice) == 0x3_ht);
    CHECK(v[Alice] == 0x3_ht);
    CHECK(v[kMeSourceID] == 0x4_ht);
    CHECK(v[Zegpold] == 0_ht);

    CHECK(v.isAbsolute() == false);
    v.makeAbsolute(Bob);
    CHECK(v.isAbsolute() == true);
    CHECK(v.asASCII()
          == "4@BobBobBobBobBobBobBobA; 3@AliceAliceAliceAliceAA, 2@DaveDaveDaveDaveDaveDA, "
             "1@CarolCarolCarolCarolCA");

    CHECK(v.updateClock(clock, true));
    CHECK(clock.state() == 4);
    v.addNewVersion(clock, Bob);
    CHECK(v.asASCII()
          == "5@BobBobBobBobBobBobBobA; 3@AliceAliceAliceAliceAA, 2@DaveDaveDaveDaveDaveDA, 1@"
             "CarolCarolCarolCarolCA");
    v.addNewVersion(clock, Dave);
    CHECK(v.asASCII()
          == "6@DaveDaveDaveDaveDaveDA; 5@BobBobBobBobBobBobBobA, 3@AliceAliceAliceAliceAA, 1@"
             "CarolCarolCarolCarolCA");
    v.addNewVersion(clock, Zegpold);
    CHECK(v.asASCII()
          == "7@ZegpoldZegpoldZegpoldA; 6@DaveDaveDaveDaveDaveDA, 5@BobBobBobBobBobBobBobA, 3@"
             "AliceAliceAliceAliceAA, 1@CarolCarolCarolCarolCA");
}

TEST_CASE("VersionVector With HybridClock", "[RevIDs]") {
    HybridClock   clock;
    VersionVector v;
    v.addNewVersion(clock, kMeSourceID);
    cout << v << endl;
    std::this_thread::sleep_for(1ms);
    v.addNewVersion(clock, Alice);
    std::this_thread::sleep_for(1ms);
    v.addNewVersion(clock, Bob);
    v.addNewVersion(clock, Dave);
    std::this_thread::sleep_for(1ms);
    v.addNewVersion(clock, Zegpold);

    auto ascii = v.asASCII(), binary = v.asBinary();
    cout << "ASCII is " << ascii.size << " bytes:  " << ascii << endl;
    cout << "Binary is " << binary.size << " bytes: " << binary << ", " << (binary.size / double(ascii.size) * 100.0)
         << "% the size\n";

    VersionVector v2 = VersionVector::fromBinary(binary);
    CHECK(v2 == v);
}

TEST_CASE("VersionVector comparison", "[RevIDs]") {
    VersionVector vEmpty;
    CHECK(vEmpty == vEmpty);
    VersionVector c1 = "1@CarolCarolCarolCarolCA"_vv;
    CHECK(c1 == c1);
    VersionVector d1 = "1@DaveDaveDaveDaveDaveDA, 2@CarolCarolCarolCarolCA"_vv;
    CHECK(c1 < d1);
    CHECK(d1 > c1);

    VersionVector c2 = "2@CarolCarolCarolCarolCA"_vv;
    CHECK(c2 < d1);
    CHECK(c2 > c1);

    VersionVector z4 = "4@ZegpoldZegpoldZegpoldA, 1@CarolCarolCarolCarolCA"_vv;
    CHECK(d1 % z4);
    CHECK(z4 % d1);
    CHECK(z4 > c1);
    CHECK(z4 % c2);
}

TEST_CASE("VersionVector conflicts", "[RevIDs]") {
    HybridClock clock;
    clock.setSource(make_unique<FakeClockSource>(0, 0));

    VersionVector v1 = "6@*;2@AliceAliceAliceAliceAA,1@DaveDaveDaveDaveDaveDA,2@CarolCarolCarolCarolCA"_vv;
    CHECK(v1 == v1);
    CHECK(v1 == "6@*;2@AliceAliceAliceAliceAA,1@DaveDaveDaveDaveDaveDA,2@CarolCarolCarolCarolCA"_vv);

    CHECK(v1 > "5@*;2@AliceAliceAliceAliceAA,1@DaveDaveDaveDaveDaveDA,2@CarolCarolCarolCarolCA"_vv);
    CHECK(v1 > "2@AliceAliceAliceAliceAA;1@DaveDaveDaveDaveDaveDA,2@CarolCarolCarolCarolCA"_vv);
    CHECK(v1 > "1@CarolCarolCarolCarolCA"_vv);
    CHECK(v1 > VersionVector());

    CHECK(v1 < "2@DaveDaveDaveDaveDaveDA;6@*,2@AliceAliceAliceAliceAA,2@CarolCarolCarolCarolCA"_vv);
    CHECK(v1
          < "2@DaveDaveDaveDaveDaveDA;1@666666666666666666666A,6@*,2@AliceAliceAliceAliceAA,9@CarolCarolCarolCarolCA"_vv);

    auto v3 = "4@AliceAliceAliceAliceAA;1@DaveDaveDaveDaveDaveDA,2@CarolCarolCarolCarolCA"_vv;

    CHECK(v1.compareTo(v3) == kConflicting);
    CHECK(!(v1 == v3));
    CHECK(!(v1 < v3));
    CHECK(!(v1 > v3));

    // Merge them:
    auto v13 = VersionVector::merge(v1, v3, clock);
    CHECK(v13.asASCII() == "7@*, 6@*, 4@AliceAliceAliceAliceAA; 2@CarolCarolCarolCarolCA, 1@DaveDaveDaveDaveDaveDA");
    CHECK(v13.isMerge());
    CHECK(v13.currentVersions() == 3);
    CHECK(v13[kMeSourceID] == 7_ht);

    auto merged = v13.mergedVersions();
    REQUIRE(merged.size() == 2);
    CHECK(merged[0] == v13[1]);
    CHECK(merged[1] == v13[2]);

    // Check that merge-related methods do the right thing on non-merges:
    CHECK(!v1.isMerge());
    CHECK(v1.currentVersions() == 1);
    CHECK(v1.mergedVersions().empty());

    VersionVector vEmpty;
    CHECK(!vEmpty.isMerge());
    CHECK(vEmpty.currentVersions() == 0);
    CHECK(vEmpty.mergedVersions().empty());
}

TEST_CASE("VersionVector update merge with two by me", "[RevIDs]") {
    HybridClock clock;
    clock.setSource(make_unique<FakeClockSource>(0, 0));
    auto vv = "7@*, 6@*, 4@AliceAliceAliceAliceAA; 2@CarolCarolCarolCarolCA, 1@DaveDaveDaveDaveDaveDA"_vv;
    // Update the version normally; there should only be one Version by me:
    vv.addNewVersion(clock);
    CHECK(vv.asASCII() == "8@*; 4@AliceAliceAliceAliceAA, 2@CarolCarolCarolCarolCA, 1@DaveDaveDaveDaveDaveDA");
}

TEST_CASE("VersionVector update merge with two by other", "[RevIDs]") {
    HybridClock clock;
    clock.setSource(make_unique<FakeClockSource>(0, 0));
    auto vv =
            "7@ZegpoldZegpoldZegpoldA, 6@ZegpoldZegpoldZegpoldA, 4@AliceAliceAliceAliceAA; 2@CarolCarolCarolCarolCA, 1@DaveDaveDaveDaveDaveDA"_vv;
    // Update the version normally; there should only be one Version by Zegpold:
    vv.addNewVersion(clock);
    CHECK(vv.asASCII()
          == "1@*; 7@ZegpoldZegpoldZegpoldA, 4@AliceAliceAliceAliceAA, 2@CarolCarolCarolCarolCA, "
             "1@DaveDaveDaveDaveDaveDA");
}

// Special case where all Versions are part of the conflict
TEST_CASE("VersionVector all-conflicts", "[RevIDs]") {
    HybridClock clock;
    clock.setSource(make_unique<FakeClockSource>(0, 0));

    auto v1 = "1@AliceAliceAliceAliceAA"_vv, v2 = "2@BobBobBobBobBobBobBobA"_vv;
    auto v12 = VersionVector::merge(v1, v2, clock);
    // ASCII form requires a trailing ';' to distinguish it from a non-merge vector:
    CHECK(v12.asASCII() == "1@*, 2@BobBobBobBobBobBobBobA, 1@AliceAliceAliceAliceAA;");
    CHECK(v12.isMerge());
    CHECK(v12.currentVersions() == 3);

    // Parse the trailing-';' form:
    VersionVector vv = VersionVector::fromASCII(v12.asASCII());
    CHECK(vv.isMerge());
    CHECK(vv.currentVersions() == 3);
    CHECK(vv.asASCII() == v12.asASCII());
}

TEST_CASE("VersionVector deltas", "[RevIDs]") {
    auto testGoodDelta = [&](const VersionVector& src, const VersionVector& dst) {
        INFO("src = '" << src << "' ; dst = '" << dst << "'");
        auto delta = dst.deltaFrom(src);
        REQUIRE(delta);
        Log("delta = '%.*s'", SPLAT(delta->asASCII()));
        CHECK(src.byApplyingDelta(*delta) == dst);
    };

    auto testBadDelta = [&](const VersionVector& src, const VersionVector& dst) {
        INFO("src = '" << src << "' ; dst = '" << dst << "'");
        auto delta = dst.deltaFrom(src);
        CHECK(!delta);
    };

    testGoodDelta(""_vv, "4@aaaaaaaaaaaaaaaaaaaaaA, 1@bbbbbbbbbbbbbbbbbbbbbA, 2@cccccccccccccccccccccA"_vv);
    testGoodDelta("4@aaaaaaaaaaaaaaaaaaaaaA, 1@bbbbbbbbbbbbbbbbbbbbbA, 2@cccccccccccccccccccccA"_vv,
                  "4@aaaaaaaaaaaaaaaaaaaaaA, 1@bbbbbbbbbbbbbbbbbbbbbA, 2@cccccccccccccccccccccA"_vv);
    testGoodDelta(
            "4@aaaaaaaaaaaaaaaaaaaaaA, 1@bbbbbbbbbbbbbbbbbbbbbA, 2@cccccccccccccccccccccA"_vv,
            "3@cccccccccccccccccccccA, 1@dddddddddddddddddddddA,4@aaaaaaaaaaaaaaaaaaaaaA, 1@bbbbbbbbbbbbbbbbbbbbbA"_vv);
    testGoodDelta(
            "4@aaaaaaaaaaaaaaaaaaaaaA,1@bbbbbbbbbbbbbbbbbbbbbA,2@cccccccccccccccccccccA"_vv,
            "3@cccccccccccccccccccccA,5@aaaaaaaaaaaaaaaaaaaaaA,1@dddddddddddddddddddddA,1@bbbbbbbbbbbbbbbbbbbbbA"_vv);

    testBadDelta("4@aaaaaaaaaaaaaaaaaaaaaA,1@bbbbbbbbbbbbbbbbbbbbbA,2@cccccccccccccccccccccA"_vv, ""_vv);
    testBadDelta("4@aaaaaaaaaaaaaaaaaaaaaA,1@bbbbbbbbbbbbbbbbbbbbbA,2@cccccccccccccccccccccA"_vv,
                 "1@bbbbbbbbbbbbbbbbbbbbbA,2@cccccccccccccccccccccA"_vv);
    testBadDelta("4@aaaaaaaaaaaaaaaaaaaaaA,1@bbbbbbbbbbbbbbbbbbbbbA,2@cccccccccccccccccccccA"_vv,
                 "5@aaaaaaaaaaaaaaaaaaaaaA"_vv);
}

TEST_CASE("VersionVector prune", "[RevIDs]") {
    auto v = "7@ZegpoldZegpoldZegpoldA; 6@DaveDaveDaveDaveDaveDA, 5@BobBobBobBobBobBobBobA, "
             "3@AliceAliceAliceAliceAA, 1@CarolCarolCarolCarolCA"_vv;

    // no-op
    auto v1 = v;
    v1.prune(999);
    CHECK(v1.count() == v.count());

    // as small as possible:
    v1 = v;
    v1.prune(0);
    CHECK(v1.asASCII() == "7@ZegpoldZegpoldZegpoldA");

    // in between:
    v1 = v;
    v1.prune(3);
    CHECK(v1.asASCII() == "7@ZegpoldZegpoldZegpoldA; 6@DaveDaveDaveDaveDaveDA, 5@BobBobBobBobBobBobBobA");

    // use a `before` time:
    v1 = v;
    v1.prune(2, logicalTime(4));
    CHECK(v1.asASCII() == "7@ZegpoldZegpoldZegpoldA; 6@DaveDaveDaveDaveDaveDA, 5@BobBobBobBobBobBobBobA");
}

#pragma mark - REVID:

struct DigestTestCase {
    const char* str;
    uint64_t    time;
    slice       digest;
    const char* hex;
};

TEST_CASE("RevID Parsing", "[RevIDs]") {
    static constexpr DigestTestCase kCases[] = {
            // good:
            {"1-aa", 1, "\xaa", "01aa"},
            {"97-beef", 97, "\xbe\xef", "61beef"},
            {"1-1234567890abcdef", 1, "\x12\x34\x56\x78\x90\xab\xcd\xef", "011234567890abcdef"},
            {"123456-1234567890abcdef", 123456, "\x12\x34\x56\x78\x90\xab\xcd\xef", "c0c4071234567890abcdef"},
            {"1234-d4596393df73462bbda0b9f8982c66a2", 1234,
             "\xd4\x59\x63\x93\xdf\x73\x46\x2b\xbd\xa0\xb9\xf8\x98\x2c\x66\xa2",
             "d209d4596393df73462bbda0b9f8982c66a2"},
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
            {"12345678123456789-aa"},  // time too large; below is digest too large
            {"1-"
             "deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadb"
             "eefdeadbeefdeadbeefd"
             "eadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef"},
    };

    for ( auto c4se : kCases ) {
        INFO("Testing '" << c4se.str << "'");
        revidBuffer r;
        if ( c4se.time ) {
            CHECK(r.tryParse(slice(c4se.str)));
            CHECK(!r.getRevID().isVersion());
            CHECK(r.getRevID().generation() == c4se.time);
            CHECK(r.getRevID().digest() == c4se.digest);
            CHECK(r.getRevID().expanded() == slice(c4se.str));
            CHECK(r.getRevID().hexString() == c4se.hex);
        } else {
            CHECK(!r.tryParse(slice(c4se.str)));
        }
    }
}

struct VersionTestCase {
    const char* str;
    uint64_t    time;
    SourceID    peer;
    const char* hex;
    const char* revidStr;
};

TEST_CASE("RevID Version Parsing", "[RevIDs]") {
    static const VersionTestCase kCases[] = {
            // good:
            {"1@*", 0x1, kMeSourceID, "000300"},
            {"bff@AliceAliceAliceAliceAA", 0xbff, Alice, "00ff2f1002589c78096271e02589c78096271e00"},
            {"c@BobBobBobBobBobBobBobA", 0xc, Bob, "0019100686c1a1b0686c1a1b0686c1a1b0686c"},
            {"d00d@*", 0xd00d, kMeSourceID, "009bc00600"},
            {"176cee53c5680000@*", 0x176cee53c5680000, kMeSourceID, "00d0959ee59ddb0b00"},

            {"c@BobBobBobBobBobBobBobA, bff@AliceAliceAliceAliceAA", 0xc, Bob, "0019100686c1a1b0686c1a1b0686c1a1b0686c",
             "c@BobBobBobBobBobBobBobA"},
            {"c@BobBobBobBobBobBobBobA; bff@AliceAliceAliceAliceAA", 0xc, Bob, "0019100686c1a1b0686c1a1b0686c1a1b0686c",
             "c@BobBobBobBobBobBobBobA"},

            // bad:
            {"0@AliceAliceAliceAliceAA"},     // time can't be 0
            {"1@0"},                          // SourceID can't be literal 0 (must be '*')
            {"12345678123456789@*"},          // time too large
            {"1@AliceAliceAliceAliceAlice"},  // SourceID too long
            {"1@AliceAlic!AliceAliceAA"},     // SourceID invalid base64
            {"1@AliceAliceAliceAlice"},       // SourceID too short
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
            {"1@@AliceAliceAliceAliceAA"},
            {"@1@11"},
            {"@11"},
            {"z@AliceAliceAliceAliceAA"},
            {"7@ax"},
            {" 1@AliceAliceAliceAliceAA"},
            {"1 @AliceAliceAliceAliceAA"},
            {"1@ AliceAliceAliceAliceAA"},
            {"1@A liceAliceAliceAliceAA"},
            {"1@AliceAliceAliceAliceAA "},
    };

    for ( auto c4se : kCases ) {
        INFO("Testing '" << c4se.str << "'");
        revidBuffer r;
        if ( c4se.time ) {
            CHECK(r.tryParse(slice(c4se.str)));
            CHECK(r.getRevID().isVersion());
            //CHECK(r.generation() == c4se.time);
            CHECK(r.getRevID().asVersion().time() == logicalTime{c4se.time});
            CHECK(r.getRevID().asVersion().author() == c4se.peer);
            CHECK(r.getRevID().expanded() == slice(c4se.revidStr ? c4se.revidStr : c4se.str));
            CHECK(r.getRevID().hexString() == c4se.hex);
        } else {
            CHECK(!r.tryParse(slice(c4se.str)));
        }
    }
}

TEST_CASE("RevID <-> Version", "[RevIDs]") {
    VersionVector vv     = "11@AliceAliceAliceAliceAA,2@BobBobBobBobBobBobBobA,1@666666666666666666666A"_vv;
    alloc_slice   vvData = vv.asBinary();
    revid         rev(vvData);
    CHECK(rev.isVersion());
    CHECK(rev.asVersion() == Version(17_ht, Alice));
    CHECK(rev.asVersionVector() == vv);
    CHECK(rev.expanded() == "11@AliceAliceAliceAliceAA"_sl);  // revid only looks at the current Version

    revidBuffer r(Version(17_ht, Alice));
    CHECK(r.getRevID().isVersion());
    CHECK(r.getRevID().asVersion() == Version(17_ht, Alice));
    CHECK(r.getRevID().expanded() == "11@AliceAliceAliceAliceAA"_sl);
}
