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
#include "Endian.hh"
#include "HybridClock.hh"
#include "RevTree.hh"
#include "LiteCoreTest.hh"
#include "StringUtil.hh"
#include "slice_stream.hh"
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
    auto tBogusPast = 0xffffffffff_ht;
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

#pragma mark - VERSION VECTOR:

TEST_CASE("Version", "[RevIDs]") {
    CHECK(Version(1_ht, kMeSourceID).asASCII() == "1@*");

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

TEST_CASE("Legacy Version", "[RevIDs]") {
    constexpr slice oldRevID = "12345-e0c8012361e94df6a1e1c2977169480e";
    static_assert(12345 == 0x3039);
    revidBuffer buf(oldRevID);
    Version     vers = Version::legacyVersion(revid(buf));
    CHECK(vers.author() == kLegacyRevSourceID);
    CHECK(vers.asASCII() == "3039e0c8012361@Revision+Tree+Encoding"_sl);
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

    v.readASCII("3@*; 2@AliceAliceAliceAliceAA,  1@DaveDaveDaveDaveDaveDA,2@CarolCarolCarolCarolCA");
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

    slice         noPVStr = "4@*, 2@AliceAliceAliceAliceAA, 1@DaveDaveDaveDaveDaveDA;"_sl;
    VersionVector noPV;
    noPV.readASCII(noPVStr);
    // The semicolon divides the current versions (the current version and merge versions) from
    // the past versions.
    CHECK(noPV.currentVersions() == 3);

    // It's ASCII representtion ends with the semicolon:
    alloc_slice noPVasASCII = noPV.asASCII();
    CHECK(noPVasASCII[noPVasASCII.size - 1] == ';');

    // The ending semicolon is optional if there is no past versions.
    VersionVector noPV2;
    noPV2.readASCII(noPVStr.upTo(noPVStr.size - 1));
    CHECK(noPV2.currentVersions() == 3);

    // However, the ASCII representation still ends with the semicolon
    CHECK(noPV2.asASCII() == noPVasASCII);

    // Special rule for noPV.currentVersions() == 1: the API genenerated
    // ASCII form does not have the ending semicolon
    slice         cv = "1@DaveDaveDaveDaveDaveDA;"_sl;
    VersionVector cvOnly;
    cvOnly.readASCII(cv);
    REQUIRE(cvOnly.currentVersions() == 1);
    REQUIRE(cvOnly.count() == 1);
    // The returned ASCII does not have the ending ";"
    CHECK(cvOnly.asASCII() == cv.upTo(cv.size - 1));
}

TEST_CASE("VersionVector <-> Binary", "[RevIDs]") {
    static constexpr uint8_t kBytes[] = {
            0x00, 0x07, 0x80, 0x03, 0x90, 0x02, 0x58, 0x9C, 0x78, 0x09, 0x62, 0x71, 0xE0, 0x25, 0x89, 0xC7, 0x80,
            0x96, 0x27, 0x1E, 0x00, 0x03, 0x90, 0x0D, 0xAB, 0xDE, 0x0D, 0xAB, 0xDE, 0x0D, 0xAB, 0xDE, 0x0D, 0xAB,
            0xDE, 0x0D, 0xAB, 0xDE, 0x0C, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x10, 0x09,
            0xAA, 0xE8, 0x94, 0x26, 0xAB, 0xA2, 0x50, 0x9A, 0xAE, 0x89, 0x42, 0x6A, 0xBA, 0x25, 0x08};
    const slice   kBinary(kBytes, sizeof(kBytes));
    VersionVector v;
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

TEST_CASE("VersionVector conflicts", "[RevIDs][Conflict]") {
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

TEST_CASE("VersionVector trivial merge", "[RevIDs][Conflict]") {
    VersionVector v1 = "6@*;2@AliceAliceAliceAliceAA,1@DaveDaveDaveDaveDaveDA,2@CarolCarolCarolCarolCA"_vv;
    VersionVector v3 = "4@AliceAliceAliceAliceAA;1@DaveDaveDaveDaveDaveDA,2@CarolCarolCarolCarolCA"_vv;
    CHECK(v1.compareTo(v3) == kConflicting);

    VersionVector v13 = VersionVector::trivialMerge(v1, v3);
    CHECK(v13.asASCII() == "6@*; 4@AliceAliceAliceAliceAA, 2@CarolCarolCarolCarolCA, 1@DaveDaveDaveDaveDaveDA");
    CHECK_FALSE(v13.isMerge());
    CHECK(v13.current() == v1.current());
    CHECK(v13.compareTo(v1) == kSame);  // it counts as the same bc the current Version matches
    CHECK(v13.compareTo(v3) == kNewer);

    // Other way round:
    VersionVector v31 = VersionVector::trivialMerge(v3, v1);
    CHECK(v31.asASCII() == "4@AliceAliceAliceAliceAA; 6@*, 2@CarolCarolCarolCarolCA, 1@DaveDaveDaveDaveDaveDA");
    CHECK_FALSE(v31.isMerge());
    CHECK(v31.current() == v3.current());
    CHECK(v31.compareTo(v1) == kNewer);
    CHECK(v31.compareTo(v3) == kSame);
}

TEST_CASE("VersionVector trivial merge of merge", "[RevIDs][Conflict]") {
    VersionVector m1 = "6@*, 5@*, 2@AliceAliceAliceAliceAA; 1@DaveDaveDaveDaveDaveDA, 2@CarolCarolCarolCarolCA"_vv;
    VersionVector m2 = "4@DaveDaveDaveDaveDaveDA; 2@CarolCarolCarolCarolCA"_vv;
    {
        // The winner is a merged vector:
        VersionVector m12 = VersionVector::trivialMerge(m1, m2);
        CHECK(m12.asASCII()
              == "6@*, 5@*, 2@AliceAliceAliceAliceAA; 4@DaveDaveDaveDaveDaveDA, 2@CarolCarolCarolCarolCA");
        CHECK(m12.isMerge());
        CHECK(m12.current() == m1.current());
        CHECK(m12.compareTo(m1) == kSame);
        CHECK(m12.compareTo(m2) == kNewer);
    }
    {
        // Other way round:
        VersionVector m21 = VersionVector::trivialMerge(m2, m1);
        CHECK(m21.asASCII() == "4@DaveDaveDaveDaveDaveDA; 6@*, 2@AliceAliceAliceAliceAA, 2@CarolCarolCarolCarolCA");
        CHECK_FALSE(m21.isMerge());
        CHECK(m21.current() == m2.current());
        CHECK(m21.compareTo(m2) == kSame);
        CHECK(m21.compareTo(m1) == kNewer);
    }
    {
        // Now the annoying case where loser has revisions newer than ones in the winner's MV,
        // so the result can't be a merge:
        VersionVector m3  = "4@AliceAliceAliceAliceAA, 2@CarolCarolCarolCarolCA"_vv;
        VersionVector m13 = VersionVector::trivialMerge(m1, m3);
        CHECK(m13.asASCII() == "6@*; 4@AliceAliceAliceAliceAA, 2@CarolCarolCarolCarolCA, 1@DaveDaveDaveDaveDaveDA");
        CHECK(!m13.isMerge());
        CHECK(m13.current() == m1.current());
        CHECK(m13.compareTo(m1) == kSame);
        CHECK(m13.compareTo(m3) == kNewer);
    }
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
    CHECK(v12.asASCII() == "3@*, 2@BobBobBobBobBobBobBobA, 1@AliceAliceAliceAliceAA;");
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

TEST_CASE("Tree RevID -> Version") {
    uint8_t const sha[20] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 16, 18, 19, 20};
    revidBuffer   rev(0xBC, slice(&sha, sizeof(sha)));
    Version       v = Version::legacyVersion(rev.getRevID());
    CHECK(v.author() == kLegacyRevSourceID);
    CHECK(uint64_t(v.time()) == 0x0000BC0102030405);
}

TEST_CASE("Valid Timestamp", "[RevIDs]") {
    constexpr slice oldRevID = "1-345d1331e65b3d965502c924d70e12337e0ea966";
    revidBuffer     buf(oldRevID);
    Version         vers = Version::legacyVersion(revid(buf));
    CHECK(vers.author() == kLegacyRevSourceID);

    // Encoding of timestamp

    uint64_t n  = 0x963d5be631135d34;              // First 8 bytes in little endian.
    uint64_t n2 = fleece::endian::dec64(n) >> 24;  // shift out 3 bytes in big endian representation.
    CHECK(n2 == 0x345d1331e6UL);                   // we are little endian
    uint64_t synthesizedTime = n2 | (uint64_t(1) << 40);
    CHECK(synthesizedTime == 0x1345d1331e6UL);

    CHECK(vers.asASCII() == "1345d1331e6@Revision+Tree+Encoding"_sl);

    HybridClock clock;
    clock.setSource(make_unique<RealClockSource>());
    CHECK(clock.see(vers.time()));  // clock.see returns false if time is not valid.

    // Theoretical minimum
    constexpr slice minRevID = "1-00";
    revidBuffer     minbuf(minRevID);
    Version         minvers = Version::legacyVersion(revid(minbuf));
    CHECK(minvers.asASCII() == "10000000000@Revision+Tree+Encoding"_sl);
    CHECK(clock.see(minvers.time()));
}

// The sourceID is a base-64 encoded 128-bit binary data. The length must by 22 bytes.
static std::string padTo22(string vv) {
    std::stringstream ss;
    while ( vv.length() > 0 ) {
        auto p = vv.find('@');
        if ( p != string::npos ) {
            ss << vv.substr(0, p + 1);
            vv          = vv.substr(p + 1);
            auto e      = vv.find_first_of(",;");
            auto source = vv.substr(0, e);
            Assert(source.length() <= 22);
            if ( source.length() < 22 ) {
                ss << source << string(21 - source.length(), '+');
                ss << "Q";
            } else {
                ss << vv.substr(0, 22);
            }
            if ( e != string::npos ) {
                ss << vv[e];
                vv = vv.substr(e + 1);
            } else {
                vv = vv.substr(vv.length());
            }
        } else {
            ss << vv;
        }
    }
    return ss.str();
}

// Cf. TestHLVIsDominating in sync_gateway
TEST_CASE("CompareTo vs IsDominating") {
    struct {
        string name;
        string hlvA;
        string hlvB;
        bool   expectedResult;
        // SGW test: hlvA.isDominating(hlvB),
    } testCases[]{{
                          "Matching current source, newer version",
                          "20@cluster1;2@cluster2",
                          "10@cluster1;1@cluster2",
                          true,
                  },
                  {
                          "Matching current source and version",
                          "20@cluster1;2@cluster2",
                          "20@cluster1;2@cluster2",
                          true,
                  },
                  {
                          "B CV found in A's PV",
                          "20@cluster1;10@cluster2",
                          "10@cluster2;15@cluster1",
                          true,
                  },
                  {
                          "B CV older than A's PV for same source",
                          "20@cluster1;15@cluster2",
                          "10@cluster2;15@cluster1",
                          true,
                  },
                  {
                          "Unique sources in A",
                          "20@cluster1;15@cluster2,3@cluster3",
                          "10@cluster2;10@cluster1",
                          true,
                  },
                  {
                          "Unique sources in B",
                          "20@cluster1",
                          "15@cluster1;3@cluster3",
                          true,
                  },
                  {
                          "B has newer cv",
                          "10@cluster1",
                          "15@cluster1",
                          false,
                  },
                  {
                          "B has newer cv than A pv",
                          "20@cluster2;10@cluster1",
                          "15@cluster1;20@cluster2",
                          false,
                  },
                  {
                          "B's cv not found in A",
                          "20@cluster2;10@cluster1",
                          "5@cluster3",
                          false,
                  },
                  {
                          "a.MV dominates B.CV",
                          "20@cluster1,20@cluster2,5@cluster3",
                          "10@cluster2",
                          true,
                  },
                  {
                          "a.MV doesn't dominate B.CV",
                          "20@cluster1,5@cluster2,5@cluster3",
                          "10@cluster2",
                          false,
                  },
                  {
                          "b.CV.source occurs in both a.CV and a.MV, dominates both",
                          "2@cluster1,1@cluster1,3@cluster2",
                          "4@cluster1",
                          false,
                  },
                  {
                          "b.CV.source occurs in both a.CV and a.MV, dominates only a.MV",
                          "4@cluster1,1@cluster1,2@cluster2",
                          "3@cluster1",
                          true,
                  }};

    auto isDominating = [](versionOrder vo) -> bool { return vo == kSame || vo == kNewer; };

    for ( const auto& test : testCases ) {
        printf("----- %s\n", test.name.c_str());
        VersionVector a   = VersionVector::fromASCII(padTo22(test.hlvA));
        VersionVector b   = VersionVector::fromASCII(padTo22(test.hlvB));
        auto          cmp = a.compareTo(b);
        CHECK(isDominating(cmp) == test.expectedResult);
    }
}

// HLVConflictStatus returns whether two HLVs are in conflict or not
enum HLVConflictStatus : int16_t {
    // HLVNoConflict indicates the two HLVs are not in conflict.
    HLVNoConflict = 1,
    // HLVConflict indicates the two HLVs are in conflict.
    HLVConflict,
    // HLVNoConflictRevAlreadyPresent indicates the two HLVs are not in conflict, but the incoming HLV does not have any
    // newer versions to add to the local HLV
    HLVNoConflictRevAlreadyPresent
};

// Cf.  TestHLVIsInConflict in sync_gateway
TEST_CASE("CompareTo vs IsInConflict") {
    struct {
        string            name;
        string            localHLV;
        string            incomingHLV;
        HLVConflictStatus conflict;
        // SGW test: IsInConflict(ctx context.Context, localHLV, incomingHLV *HybridLogicalVector)
    } testCases[]{
            {
                    "CV equal",
                    "111@abc;123@def",
                    "111@abc;123@ghi",
                    HLVNoConflictRevAlreadyPresent,
            },
            {
                    "no conflict case",
                    "111@abc;123@def",
                    "112@abc;123@ghi",
                    HLVNoConflict,
            },
            {
                    "local revision is newer",
                    "111@abc;123@def",
                    "100@abc;123@ghi",
                    HLVNoConflictRevAlreadyPresent,
            },
            {
                    "merge versions match",
                    "130@abc,123@def,100@ghi;50@jkl",
                    "150@mno,123@def,100@ghi;50@jkl",
                    HLVNoConflict,
            },
            {
                    "cv conflict",
                    "1@abc",
                    "1@def",
                    HLVConflict,
            },
            {
                    "Matching current source, newer version",
                    "20@cluster1;2@cluster2",
                    "10@cluster1;1@cluster2",
                    HLVNoConflictRevAlreadyPresent,
            },
            {
                    "Matching current source and version",
                    "20@cluster1;2@cluster2",
                    "20@cluster1;2@cluster2",
                    HLVNoConflictRevAlreadyPresent,
            },
            {
                    "B CV found in A's PV",
                    "20@cluster1;10@cluster2",
                    "10@cluster2;15@cluster1",
                    HLVNoConflictRevAlreadyPresent,
            },
            {
                    "B CV older than A's PV for same source",
                    "20@cluster1;15@cluster2",
                    "10@cluster2;15@cluster1",
                    HLVNoConflictRevAlreadyPresent,
            },
            {
                    "Unique sources in A",
                    "20@cluster1;15@cluster2,3@cluster3",
                    "10@cluster2;10@cluster1",
                    HLVNoConflictRevAlreadyPresent,
            },
            {
                    "Unique sources in B",
                    "20@cluster1",
                    "15@cluster1;3@cluster3",
                    HLVNoConflictRevAlreadyPresent,
            },
            {
                    "B has newer cv than A pv",
                    "20@cluster2;10@cluster1",
                    "15@cluster1;20@cluster2",
                    HLVNoConflict,
            },
            {
                    "B's cv not found in A",
                    "20@cluster2;10@cluster1",
                    "5@cluster3",
                    HLVConflict,
            },
            {
                    "a.MV dominates B.CV",
                    "20@cluster1,20@cluster2,5@cluster3",
                    "10@cluster2",
                    HLVNoConflictRevAlreadyPresent,
            },
            {
                    "a.MV doesn't dominate B.CV", "20@cluster1,5@cluster2,5@cluster3", "10@cluster2",
                    HLVConflict,  // conflict since mv doesn't match
            },
            {
                    "b.CV.source occurs in both a.CV and a.MV, dominates both",
                    "2@cluster1,1@cluster1,3@cluster2",
                    "4@cluster1",
                    HLVNoConflict,
            },
            {
                    "b.CV.source occurs in both a.CV and a.MV, dominates only a.MV",
                    "4@cluster1,1@cluster1,2@cluster2",
                    "3@cluster1",
                    HLVNoConflictRevAlreadyPresent,
            },
    };

    std::set<string> failingTests{// Test based only on the merge versions.
                                  "merge versions match"};

    for ( const auto& test : testCases ) {
        printf("----- %s\n", test.name.c_str());

        VersionVector a   = VersionVector::fromASCII(padTo22(test.localHLV));
        VersionVector b   = VersionVector::fromASCII(padTo22(test.incomingHLV));
        auto          cmp = a.compareTo(b);

        if ( failingTests.contains(test.name) ) {
            // skip this case now.
            // printf("test.conflict(%d) vs cmp(%d)\n", test.conflict, cmp);
            continue;
        }
        switch ( test.conflict ) {
            case HLVNoConflict:
                CHECK(cmp == kOlder);
                break;
            case HLVConflict:
                CHECK(cmp == kConflicting);
                break;
            case HLVNoConflictRevAlreadyPresent:
                CHECK((cmp == kNewer || cmp == kSame));
                break;
            default:
                break;
        }
    }
}

// Cf. TestHLVUpdateFromIncoming in sync_gateway
TEST_CASE("trivialMerge vs UpdateWithIncomingHLV") {
    struct {
        string name;
        string existingHLV;
        string incomingHLV;
        string finalHLV;
        // SGW test: localHLV.UpdateWithIncomingHLV(incomingHLV) == finalHLV
    } testCases[] = {
            {
                    "update cv and add pv",
                    "15@abc",
                    "25@def;20@abc",
                    "25@def;20@abc",
            },
            {
                    "update cv, move cv to pv",
                    "15@abc;30@def",
                    "35@def;15@abc",
                    "35@def;15@abc",
            },
            {
                    "Add new MV",
                    "",
                    "1@b,1@a,2@c",
                    "1@b,1@a,2@c",
            },
            {
                    "existing mv, move to pv",
                    "3@c,2@b,1@a",
                    "4@c",
                    "4@c;2@b,1@a",
            },
            {
                    "incoming pv overwrite mv, equal values",
                    "3@c,2@b,1@a",
                    "4@c;2@b,1@a",
                    "4@c;2@b,1@a",
            },
            {
                    "incoming mv overwrite pv, equal values",
                    "3@c;2@b,1@a",
                    "4@c,2@b,1@a",
                    "4@c,2@b,1@a",
            },
            {
                    "incoming mv overwrite pv, greater values",
                    "3@c;2@b,1@a",
                    "4@c,5@b,6@a",
                    "4@c,5@b,6@a",
            },
            // Invalid MV cleanup cases should preserve any conflicting versions from incoming HLV
            {
                    // Invalid since MV should always have two values.
                    "Add single value MV",
                    "",
                    "1@b,1@a",
                    "1@b,1@a",
            },
            {
                    // Invalid since there should not be able to be an incoming merge conflict where a different newer version exists.
                    "incoming mv partially overlaps with pv",
                    "3@c;2@b,6@a",
                    "4@c,2@b,1@a",
                    "4@c,2@b,1@a",
            },
            {
                    "incoming doc has MV existing does not", "10@xyz;8@foo,9@bar", "20@abc,10@def,11@efg;5@foo",
                    //            "20@abc,10@def,11@efg;10@xyz,8@foo,9@bar",
                    "20@abc,10@def,11@efg;10@xyz,9@bar,8@foo",  // canonical order
            },
            {
                    "incoming HLV had CV in common with existing HLV PV",
                    "11@xyz;7@foo,10@abc",
                    "20@abc;5@foo",
                    "20@abc;11@xyz,7@foo",
            },
            {
                    "existing HLV had CV in common with incoming HLV PV",
                    "11@xyz;7@foo",
                    "20@abc;5@foo,10@xyz",
                    "20@abc;11@xyz,7@foo",
            },
            {
                    "incoming hlv has MV entry less than existing hlv", "2@xyz,8@def,9@efg;1@foo",
                    "10@abc,1@def,3@efg;1@foo",
                    //            "10@abc;2@xyz,8@def,9@efg,1@foo",
                    "10@abc;9@efg,8@def,2@xyz,1@foo",  // canonical order
            },
            {
                    "incoming hlv has PV entry less than existing hlv PV entry", "2@xyz;8@def,9@efg,4@foo",
                    "10@abc;1@foo,3@def",
                    //            "10@abc;2@xyz,4@foo,8@def,9@efg",
                    "10@abc;9@efg,8@def,4@foo,2@xyz",  // canonical order
            },
            {
                    "local hlv has MV entry greater than remote hlv",
                    "2@xyz,8@def,9@efg;1@bar",
                    "10@abc,10@def,11@efg;1@foo",
                    "10@abc,10@def,11@efg;2@xyz,1@bar,1@foo",
            },
            {
                    "local hlv has PV entry greater than remote hlv PV entry", "2@xyz;8@def,9@efg",
                    "10@abc;10@foo,11@def",
                    //            "10@abc;2@xyz,11@def,9@efg,10@foo",
                    "10@abc;11@def,10@foo,9@efg,2@xyz,",  // canonical order
            },
            {
                    "both local and remote have mv and no common sources", "2@xyz,8@lmn,9@pqr;1@bar",
                    "10@abc,10@def,11@efg;1@foo",
                    //            "10@abc,10@def,11@efg;2@xyz,8@lmn,9@pqr,1@bar,1@foo",
                    "10@abc,10@def,11@efg;9@pqr,8@lmn,2@xyz,1@bar,1@foo",  // canonical order
            },
            {
                    "both local and remote have no common sources in PV",
                    "20@xyz;2@bar",
                    "10@abc;1@foo",
                    "10@abc;20@xyz,2@bar,1@foo",
            },
            {
                    "local hlv has cv less than remote hlv",
                    "20@xyz;2@foo",
                    "10@abc;1@foo",
                    "10@abc;20@xyz,2@foo",
            },
    };

    std::set<string> failingTests{
            // In SGW, the mvs of the incomingHLV are kept unless any of them dominated by
            // the mvs of existingHLV
            "incoming mv partially overlaps with pv",
    };

    for ( const auto& test : testCases ) {
        printf("----- %s\n", test.name.c_str());

        VersionVector a = VersionVector::fromASCII(padTo22(test.existingHLV));
        VersionVector b = VersionVector::fromASCII(padTo22(test.incomingHLV));
        VersionVector c = VersionVector::fromASCII(padTo22(test.finalHLV));

        VersionVector merged = VersionVector::trivialMerge(b, a);

        if ( failingTests.contains(test.name) ) {
            string lite = merged.asASCII().asString();
            string sgw  = c.asASCII().asString();
            // printf("FAILING %s =====\nLite=%s\nSGW=%s\n", test.name.c_str(), lite.c_str(), sgw.c_str());
            continue;
        }

        CHECK(merged.asASCII() == c.asASCII());
    }
}

// Cf.  TestHLVUpdateFromIncomingNewCV in sync_gateway
TEST_CASE("merge vs MergeWithIncomingHLV") {
    struct {
        string name;
        string existingHLV;
        string incomingHLV;
        string newCV;
        string finalHLV;
        // SGW test: localHLV.MergeWithIncomingHLV(test.newCV, incomingHLV) == finalHLV
        // Lite function: VersionVector::merge(existingHLV, incomingHLV, clock);
    } testCases[] = {
            {
                    "simple merge",
                    "1@a",
                    "2@b",
                    "3@c",  // newCV:       Version{SourceID: "c", Value: 3},
                    "3@c,2@b,1@a",
            },
            {
                    "existing mv",
#if 0
            // In LiteCore, the merge function uses the hybrid clock for the time of the new CV.
            // It bumps the time based on the times of CVs of the merged HLVs. It presumes that
            // the time of the CV is newer than the times of the accompanying MVs and PVs.
            // We adjust the time of CVs in order to make the comparison meaningful.
            "1@a,3@d,4@e",
            "2@b",
            "5@c",        // newCV:       Version{SourceID: "c", Value: 5},
            "5@c,2@b,1@a;4@e,3@d",
#else
                    "5@a,3@d,4@e",
                    "2@b",
                    "6@c",  // newCV:       Version{SourceID: "c", Value: 5},
                    "6@c,5@a,2@b;4@e,3@d",
#endif
            },
            {
                    "existing pv",
#if 0
            "1@a;3@d,4@e",
            "2@b",
            "5@c",        // newCV:       Version{SourceID: "c", Value: 5},
            "5@c,2@b,1@a;4@e,3@d",
#else
                    "5@a;3@d,4@e",
                    "2@b",
                    "6@c",  // newCV:       Version{SourceID: "c", Value: 5},
                    "6@c,5@a,2@b;4@e,3@d",
#endif
            },
            {
                    "incoming mv",
                    "1@a",
                    "4@b,3@d,2@e",
                    "5@c",  // newCV:       Version{SourceID: "c", Value: 5},
                    "5@c,4@b,1@a;3@d,2@e",
            },
            {
                    "incoming pv",
#if 0
            "1@a",
            "2@b;4@d,3@e",
            "5@c",      // newCV:       Version{SourceID: "c", Value: 5},
            "5@c,2@b,1@a;4@d,3@e",
#else
                    "1@a",
                    "5@b;4@d,3@e",
                    "6@c",  // newCV:       Version{SourceID: "c", Value: 5},
                    "6@c,5@b,1@a;4@d,3@e",
#endif
            },
            {
                    "both mv",
                    "1@a,3@d,4@e",
                    "6@b,5@f,2@g",
                    "7@c",  // newCV:       Version{SourceID: "c", Value: 7},
                    "7@c,6@b,1@a;5@f,4@e,3@d,2@g",
            },
            {
                    "both pv",
#if 0
            "1@a;3@d,4@e",
            "2@b;6@f,5@g",
            "7@c",      // newCV:       Version{SourceID: "c", Value: 7},
            "7@c,2@b,1@a;6@f,5@g,4@e,3@d",
#else
                    "5@a;3@d,4@e",
                    "7@b;6@f,5@g",
                    "8@c",  // newCV:       Version{SourceID: "c", Value: 7},
                    "8@c,7@b,5@a;6@f,5@g,4@e,3@d",
#endif
            },
            {
                    "existing mv and incoming pv",
#if 0
            "1@a,3@d,4@e",
            "2@b;6@f,5@g",
            "7@c",      // newCV:       Version{SourceID: "c", Value: 7},
            "7@c,2@b,1@a;6@f,5@g,4@e,3@d",
#else
                    "5@a,3@d,4@e",
                    "7@b;6@f,5@g",
                    "8@c",  // newCV:       Version{SourceID: "c", Value: 7},
                    "8@c,7@b,5@a;6@f,5@g,4@e,3@d",
#endif
            },
            {
                    "existing pv and incoming mv",
#if 0
            "1@a;3@d,4@e",
            "6@b,5@f,2@g",
            "7@c",      // newCV:       Version{SourceID: "c", Value: 7},
            "7@c,6@b,1@a;5@f,4@e,3@d,2@g",
#else
                    "5@a;3@d,4@e",
                    "6@b,5@f,2@g",
                    "7@c",  // newCV:       Version{SourceID: "c", Value: 7},
                    "7@c,6@b,5@a;5@f,4@e,3@d,2@g",
#endif
            },
            {
                    "existing mv,pv, incoming mv",
#if 0
            "1@a,3@d,4@e;8@h,7@g",
            "6@b,5@f,2@c",
            "9@i",      // newCV:       Version{SourceID: "i", Value: 9},
            "9@i,6@b,1@a;8@h,7@g,5@f,4@e,3@d,2@c",
#else
                    "9@a,3@d,4@e;8@h,7@g",
                    "6@b,5@f,2@c",
                    "a@i",  // newCV:       Version{SourceID: "i", Value: 9},
                    "a@i,9@a,6@b;8@h,7@g,5@f,4@e,3@d,2@c",
#endif
            },
            {
                    "existing mv,pv, incoming pv",
#if 0
            "1@a,3@d,4@e;8@h,7@g",
            "6@b;5@f,2@c",
            "9@i",      // newCV:       Version{SourceID: "i", Value: 9},
            "9@i,6@b,1@a;8@h,7@g,5@f,4@e,3@d,2@c",
#else
                    "9@a,3@d,4@e;8@h,7@g",
                    "6@b;5@f,2@c",
                    "a@i",  // newCV:       Version{SourceID: "i", Value: 9},
                    "a@i,9@a,6@b;8@h,7@g,5@f,4@e,3@d,2@c",
#endif
            },
            {
                    "existing mv,pv, incoming mv,pv",
#if 0
            "1@a,3@d,4@e;8@h,7@g",
            "6@b,5@f,2@c;9@i,10@j",
            "11@k",         // newCV:       Version{SourceID: "k", Value: 11},
            // note newCV is b@k because SourceID is always encoded
            "b@k,6@b,1@a;10@j,9@i,8@h,7@g,5@f,4@e,3@d,2@c",
#else
                    "9@a,3@d,4@e;8@h,7@g",
                    "11@b,5@f,2@c;9@i,10@j",
                    "12@k",  // newCV:       Version{SourceID: "k", Value: 11},
                    // note newCV is b@k because SourceID is always encoded
                    "12@k,11@b,9@a;10@j,9@i,8@h,7@g,5@f,4@e,3@d,2@c",
#endif
            },
            {
                    "existing mv duplicates value with existing cv",
                    "3@a,2@b,1@a",
                    "4@d",
                    "5@e",  // newCV:       Version{SourceID: "e", Value: 5},
                    "5@e,4@d,3@a;2@b",
            },
            {
                    "incoming mv duplicates value with incoming cv",
                    "1@a",
                    "4@c,3@b,2@c",
                    "5@d",  // newCV:       Version{SourceID: "d", Value: 5},
                    "5@d,4@c,1@a;3@b",
            },
    };

    auto toGlobalSourceID = [](const VersionVector& vv, const string& globalMeID) -> string {
        string strVV = vv.asASCII().asString();
        auto   star  = strVV.find("*");
        if ( star != string::npos ) strVV.replace(star, 1, globalMeID);
        return strVV;
    };

    for ( const auto& test : testCases ) {
        HybridClock clock;
        clock.setSource(make_unique<FakeClockSource>(1, 0));

        printf("----- %s\n", test.name.c_str());
        VersionVector localHLV    = VersionVector::fromASCII(padTo22(test.existingHLV));
        VersionVector incomingHLV = VersionVector::fromASCII(padTo22(test.incomingHLV));
        VersionVector expectedHLV = VersionVector::fromASCII(padTo22(test.finalHLV));

        CHECK(localHLV.compareTo(incomingHLV) == kConflicting);

        VersionVector merged               = VersionVector::merge(localHLV, incomingHLV, clock);
        auto          at                   = test.finalHLV.find("@");
        auto          finalSrcID           = test.newCV.substr(at + 1);
        auto          mergedWithFinalSrcID = padTo22(toGlobalSourceID(merged, finalSrcID));

        CHECK(mergedWithFinalSrcID == expectedHLV.asASCII().asString());
    }
}
