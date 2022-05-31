//
// SequenceTrackerTest.cc
//
// Copyright 2016-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "LiteCoreTest.hh"
#include "SequenceTracker.hh"
#include <sstream>

using namespace std;
using namespace litecore;
using namespace fleece;


inline alloc_slice operator "" _asl (const char *str, size_t length)
{return alloc_slice(str, length);}


namespace litecore {

    class SequenceTrackerTest : public TestFixture {     // SequenceTracker declares this class a friend
    public:
        SequenceTrackerTest()
        :tracker("SequenceTrackerTest")
        {
            oldMinChanges = SequenceTracker::kMinChangesToKeep;
            SequenceTracker::kMinChangesToKeep = 2;
        }

        ~SequenceTrackerTest() {
            SequenceTracker::kMinChangesToKeep = oldMinChanges;
        }

        SequenceTracker tracker;
        sequence_t seq = 0_seq;

        // These methods provide access to private members of SequenceTracker

#if DEBUG
        string dump(bool verbose =false) { return tracker.dump(verbose); }
#endif

        SequenceTracker::const_iterator since(sequence_t s) {
            return tracker._since(s);
        }

        slice docIDAt(sequence_t s) {
            return tracker._docIDAt(s);
        }
        
        SequenceTracker::const_iterator end() {
            return tracker.end();
        }

    private:
        size_t oldMinChanges;
    };
    
}


static constexpr auto Flag1 = SequenceTracker::RevisionFlags(0x11);
static constexpr auto Flag2 = SequenceTracker::RevisionFlags(0x22);
static constexpr auto Flag3 = SequenceTracker::RevisionFlags(0x33);
static constexpr auto Flag4 = SequenceTracker::RevisionFlags(0x44);
static constexpr auto Flag5 = SequenceTracker::RevisionFlags(0x55);
static constexpr auto Flag6 = SequenceTracker::RevisionFlags(0x66);
static constexpr auto Flag7 = SequenceTracker::RevisionFlags(0x77);
static constexpr auto Flag8 = SequenceTracker::RevisionFlags(0x88);
static constexpr auto Flag9 = SequenceTracker::RevisionFlags(0x99);


TEST_CASE_METHOD(litecore::SequenceTrackerTest, "SequenceTracker", "[notification]") {
    tracker.beginTransaction();
    tracker.documentChanged("A"_asl, "1-aa"_asl, ++seq, 1111, Flag1);
    tracker.documentChanged("B"_asl, "1-bb"_asl, ++seq, 2222, Flag2);
    tracker.documentChanged("C"_asl, "1-cc"_asl, ++seq, 3333, Flag3);
    REQUIRE_IF_DEBUG(dump(true) == "[(A@1#11+1111, B@2#22+2222, C@3#33+3333)]");
    CHECK(tracker.lastSequence() == seq);
    tracker.documentChanged("B"_asl, "2-bb"_asl, ++seq, 4444, Flag4);
    REQUIRE_IF_DEBUG(dump(true) == "[(A@1#11+1111, C@3#33+3333, B@4#44+4444)]");
    tracker.documentChanged("B"_asl, "3-bb"_asl, ++seq, 5555, Flag5);
    CHECK(tracker.lastSequence() == seq);
    REQUIRE_IF_DEBUG(dump(true) == "[(A@1#11+1111, C@3#33+3333, B@5#55+5555)]");
    tracker.documentChanged("A"_asl, "2-aa"_asl, ++seq, 6666, Flag6);
    CHECK(tracker.lastSequence() == seq);
    REQUIRE_IF_DEBUG(dump(true) == "[(C@3#33+3333, B@5#55+5555, A@6#66+6666)]");
    tracker.documentChanged("D"_asl, "1-dd"_asl, ++seq, 7777, Flag7);
    CHECK(tracker.lastSequence() == seq);
    REQUIRE_IF_DEBUG(dump(true) == "[(C@3#33+3333, B@5#55+5555, A@6#66+6666, D@7#77+7777)]");

    REQUIRE(docIDAt(0_seq) == "C"_sl);
    REQUIRE(docIDAt(4_seq) == "B"_sl);
    REQUIRE(docIDAt(5_seq) == "A"_sl);
    REQUIRE(docIDAt(6_seq) == "D"_sl);
    REQUIRE(since(7_seq) == end());
}


TEST_CASE_METHOD(litecore::SequenceTrackerTest, "SequenceTracker DatabaseChangeNotifier", "[notification]") {
    tracker.beginTransaction();
    tracker.documentChanged("A"_asl, "1-aa"_asl, ++seq, 1111, Flag1);
    tracker.documentChanged("B"_asl, "1-bb"_asl, ++seq, 2222, Flag2);
    tracker.documentChanged("C"_asl, "1-cc"_asl, ++seq, 3333, Flag3);

    int count1=0, count2=0, count3=0;
    CollectionChangeNotifier cn1(tracker, [&](CollectionChangeNotifier&) {++count1;});
    CollectionChangeNotifier cn2(tracker, [&](CollectionChangeNotifier&) {++count2;});
    {
        CollectionChangeNotifier cn3(tracker, [&](CollectionChangeNotifier&) {++count3;}, 1_seq);
        REQUIRE_IF_DEBUG(dump() == "[(A@1, *, B@2, C@3, *, *)]");

        SequenceTracker::Change changes[5];
        bool external;
        REQUIRE(cn3.readChanges(changes, 5, external) == 2);
        CHECK(!external);
        CHECK(changes[0].docID == "B"_sl);
        CHECK(changes[0].revID == "1-bb"_sl);
        CHECK(changes[0].sequence == 2_seq);
        CHECK(changes[1].docID == "C"_sl);
        REQUIRE_IF_DEBUG(dump() == "[(A@1, B@2, C@3, *, *, *)]");
        REQUIRE(!cn3.hasChanges());
        REQUIRE(cn3.readChanges(changes, 5, external) == 0);

        CHECK(count1==0);
        CHECK(count2==0);
        CHECK(count3==0);

        tracker.documentChanged("B"_asl, "2-bb"_asl, ++seq, 4444, Flag4);

        REQUIRE(cn1.hasChanges());
        REQUIRE(cn1.readChanges(changes, 5, external) == 1);
        CHECK(changes[0].docID == "B"_sl);
        CHECK(changes[0].revID == "2-bb"_sl);
        CHECK(changes[0].sequence == 4_seq);
        CHECK(!external);
        REQUIRE(!cn1.hasChanges());
        REQUIRE(cn1.readChanges(changes, 5, external) == 0);
        CHECK(!external);

        REQUIRE(cn2.hasChanges());

        CHECK(count1==1);
        CHECK(count2==1);
        CHECK(count3==1);

        tracker.documentChanged("C"_asl, "2-cc"_asl, ++seq, 5555, Flag5);

        CHECK(count1==2);   // was notified again because it called changes() after 1st change
        CHECK(count2==1);   // wasn't because it didn't
        CHECK(count3==1);   // ditto
        REQUIRE_IF_DEBUG(dump() == "[(A@1, *, *, B@4, *, C@5)]");
    }
    // After cn3 is destructed:
    REQUIRE_IF_DEBUG(dump() == "[(A@1, *, B@4, *, C@5)]");
}


TEST_CASE_METHOD(litecore::SequenceTrackerTest, "SequenceTracker DocChangeNotifier", "[notification]") {
    tracker.beginTransaction();
    
    std::unique_ptr<CollectionChangeNotifier> cn;

    SECTION("With db change notifier") {
        cn = make_unique<CollectionChangeNotifier>(tracker, nullptr);
    }
    SECTION("Without db change notifier") {
        // don't initialize cn. Now the tracker isn't recording document changes...
    }

    tracker.documentChanged("A"_asl, "1-aa"_asl, ++seq, 1111, Flag1);
    tracker.documentChanged("B"_asl, "1-bb"_asl, ++seq, 2222, Flag2);
    tracker.documentChanged("C"_asl, "1-cc"_asl, ++seq, 3333, Flag3);

    int countA=0, countB=0, countB2=0, countD=0;

    DocChangeNotifier cnA(tracker, "A"_sl, [&](DocChangeNotifier&, slice docID, sequence_t s) {
        CHECK(docID == "A"_sl);
        CHECK(s == seq);
        ++countA;
    });
    DocChangeNotifier cnB(tracker, "B"_sl, [&](DocChangeNotifier&, slice docID, sequence_t s) {
        CHECK(docID == "B"_sl);
        CHECK(s == seq);
        ++countB;
    });
    // Create one for a doc that doesn't exist yet:
    DocChangeNotifier cnD(tracker, "D"_sl, [&](DocChangeNotifier&, slice docID, sequence_t s) {
        CHECK(docID == "D"_sl);
        CHECK(s == seq);
        ++countD;
    });

    tracker.documentChanged("A"_asl, "2-aa"_asl, ++seq, 4444, Flag4);
    CHECK(countA==1);
    CHECK(countB==0);

    tracker.documentChanged("B"_asl, "2-bb"_asl, ++seq, 5555, Flag5);
    CHECK(countA==1);
    CHECK(countB==1);

    {
        DocChangeNotifier cnB2(tracker, "B"_sl, [&](DocChangeNotifier&,slice,sequence_t) {++countB2;});
        tracker.documentChanged("B"_asl, "3-bb"_asl, ++seq, 6666, Flag6);
        CHECK(countA==1);
        CHECK(countB==2);
        CHECK(countB2==1);
    }

    tracker.documentChanged("B"_asl, "4-bb"_asl, ++seq, 7777, Flag7);
    CHECK(countA==1);
    CHECK(countB==3);
    CHECK(countB2==1);
    CHECK(countD==0);

    tracker.documentChanged("D"_asl, "1-dd"_asl, ++seq, 8888, Flag8);
    CHECK(countA==1);
    CHECK(countB==3);
    CHECK(countB2==1);
    CHECK(countD==1);

    tracker.documentChanged("Z"_asl, "9-zz"_asl, ++seq, 999, Flag9);

    tracker.endTransaction(true);
}


TEST_CASE("SequenceTracker Transaction", "[notification]") {
    SequenceTracker tracker("test");

    SequenceTracker::Change changes[10];
    size_t numChanges;
    bool external;
    CollectionChangeNotifier cn(tracker, nullptr);

    // First create some docs:
    sequence_t seq = 0_seq;
    tracker.beginTransaction();
    tracker.documentChanged("A"_asl, "1-aa"_asl, ++seq, 1111, Flag1);
    tracker.documentChanged("B"_asl, "1-bb"_asl, ++seq, 2222, Flag2);
    tracker.documentChanged("C"_asl, "1-cc"_asl, ++seq, 3333, Flag3);
    tracker.endTransaction(true);
    CHECK_IF_DEBUG(tracker.dump() == "[*, A@1, B@2, C@3]");
    numChanges = cn.readChanges(changes, 10, external);
    REQUIRE(numChanges == 3);

    // Now start a transaction and make two more changes:
    tracker.beginTransaction();
    tracker.documentChanged("B"_asl, "2-bb"_asl, ++seq, 4444, Flag4);
    tracker.documentChanged("D"_asl, "1-dd"_asl, ++seq, 5555, Flag5);

    CHECK_IF_DEBUG(tracker.dump() == "[A@1, C@3, *, (B@4, D@5)]");

    // Start tracking individual document notifications:
    int countA=0, countB=0, countD=0;
    DocChangeNotifier cnA(tracker, "A"_sl, [&](DocChangeNotifier&,slice,sequence_t) {
        ++countA;
    });
    DocChangeNotifier cnB(tracker, "B"_sl, [&](DocChangeNotifier&,slice,sequence_t) {
        ++countB;
    });
    DocChangeNotifier cnD(tracker, "D"_sl, [&](DocChangeNotifier&,slice,sequence_t) {
        ++countD;
    });

    SECTION("Commit, then check feed") {
        // Commit:
        tracker.endTransaction(true);
        CHECK(tracker.lastSequence() == 5_seq);

        CHECK_IF_DEBUG(tracker.dump() == "[A@1, C@3, *, B@4, D@5]");

        // Make sure the committed changes appear in the feed:
        numChanges = cn.readChanges(changes, 10, external);
        REQUIRE(numChanges == 2);
        CHECK(changes[0].docID == "B"_sl);
        CHECK(changes[1].docID == "D"_sl);

        CHECK(countA == 0);
        CHECK(countB == 0);
        CHECK(countD == 0);
    }

    SECTION("Check feed, then commit") {
        // Make sure the uncommitted changes appear in the feed:
        numChanges = cn.readChanges(changes, 10, external);
        REQUIRE(numChanges == 2);
        CHECK(changes[0].docID == "B"_sl);
        CHECK(changes[1].docID == "D"_sl);
        CHECK_IF_DEBUG(tracker.dump() == "[A@1, C@3, (B@4, D@5, *)]");

        // Commit:
        tracker.endTransaction(true);
        CHECK(tracker.lastSequence() == 5_seq);

        CHECK_IF_DEBUG(tracker.dump() == "[A@1, C@3, B@4, D@5, *]");

        // The commit itself shouldn't add to the feed or change the docs:
        numChanges = cn.readChanges(changes, 10, external);
        CHECK(numChanges == 0);
        CHECK(countA == 0);
        CHECK(countB == 0);
        CHECK(countD == 0);
    }

    SECTION("Abort, then check feed") {
        tracker.endTransaction(false);
        CHECK(tracker.lastSequence() == 3_seq);
        CHECK_IF_DEBUG(tracker.dump() == "[A@1, C@3, *, B@2, D@0]");

        numChanges = cn.readChanges(changes, 10, external);
        REQUIRE(numChanges == 2);
        CHECK(changes[0].docID == "B"_sl);
        CHECK(changes[1].docID == "D"_sl);

        CHECK(countA == 0);
        CHECK(countB == 1);
        CHECK(countD == 1);
    }

    SECTION("Check feed, then abort") {
        numChanges = cn.readChanges(changes, 10, external);
        REQUIRE(numChanges == 2);
        CHECK(changes[0].docID == "B"_sl);
        CHECK(changes[1].docID == "D"_sl);
        CHECK_IF_DEBUG(tracker.dump() == "[A@1, C@3, (B@4, D@5, *)]");

        // Abort:
        tracker.endTransaction(false);
        CHECK(tracker.lastSequence() == 3_seq);
        CHECK_IF_DEBUG(tracker.dump() == "[A@1, C@3, *, B@2, D@0]");

        // The rolled-back docs should be in the feed again:
        numChanges = cn.readChanges(changes, 10, external);
        REQUIRE(numChanges == 2);
        CHECK(changes[0].docID == "B"_sl);
        CHECK(changes[1].docID == "D"_sl);

        CHECK(countA == 0);
        CHECK(countB == 1);
        CHECK(countD == 1);
    }
}


TEST_CASE_METHOD(litecore::SequenceTrackerTest, "SequenceTracker Ignores ExternalChanges", "[notification]") {
    SequenceTracker track2("track2");
    track2.beginTransaction();
    track2.documentChanged("B"_asl, "2-bb"_asl, ++seq, 4444, Flag4);
    track2.documentChanged("Z"_asl, "1-ff"_asl, ++seq, 5555, Flag5);

    // Notify tracker about the transaction from track2:
    tracker.addExternalTransaction(track2);
    track2.endTransaction(true);

    // tracker ignored the changes because it has no observers:
    CHECK_IF_DEBUG(tracker.dump() == "[]");
}


TEST_CASE_METHOD(litecore::SequenceTrackerTest, "SequenceTracker ExternalChanges", "[notification]") {
    // Add a change notifier:
    int count1 = 0;
    CollectionChangeNotifier cn(tracker, [&](CollectionChangeNotifier&) {++count1;}, 0_seq);

    // Add some docs:
    tracker.beginTransaction();
    tracker.documentChanged("A"_asl, "1-aa"_asl, ++seq, 1111, Flag1);
    tracker.documentChanged("B"_asl, "1-bb"_asl, ++seq, 2222, Flag2);
    tracker.documentChanged("C"_asl, "1-cc"_asl, ++seq, 3333, Flag3);
    tracker.endTransaction(true);

    // notifier was notified:
    CHECK(count1 == 1);

    SequenceTracker track2("track2");
    track2.beginTransaction();
    track2.documentChanged("B"_asl, "2-bb"_asl, ++seq, 4444, Flag4);
    track2.documentChanged("Z"_asl, "1-ff"_asl, ++seq, 5555, Flag5);

    // Notify tracker about the transaction from track2:
    tracker.addExternalTransaction(track2);
    track2.endTransaction(true);

    // tracker added the changes because it has an observer:
    CHECK_IF_DEBUG(tracker.dump() == "[*, A@1, C@3, B@4', Z@5']");

    // notifier wasn't up to date before the transaction, so its callback didn't get called again:
    CHECK(count1 == 1);

    SequenceTracker::Change changes[10];
    bool external;
    size_t numChanges = cn.readChanges(changes, 10, external);
    REQUIRE(numChanges == 2);
    CHECK(external == false);
    CHECK(changes[0].docID == "A"_sl);
    CHECK(changes[1].docID == "C"_sl);
    numChanges = cn.readChanges(changes, 10, external);
    REQUIRE(numChanges == 2);
    CHECK(external == true);
    CHECK(changes[0].docID == "B"_sl);
    CHECK(changes[1].docID == "Z"_sl);
}


TEST_CASE_METHOD(litecore::SequenceTrackerTest, "SequenceTracker Purge", "[notification]") {
    int count1=0;
    CollectionChangeNotifier cn1(tracker, [&](CollectionChangeNotifier&) {++count1;});

    tracker.beginTransaction();
    tracker.documentChanged("A"_asl, "1-aa"_asl, ++seq, 1111, Flag1);
    tracker.documentChanged("B"_asl, "1-bb"_asl, ++seq, 2222, Flag2);
    tracker.documentPurged("A"_sl);

    {
        REQUIRE_IF_DEBUG(dump() == "[*, (B@2, A@0)]");
        SequenceTracker::Change changes[5];
        bool external;
        REQUIRE(cn1.readChanges(changes, 5, external) == 2);
        CHECK(!external);
        CHECK(changes[0].docID == "B"_sl);
        CHECK(changes[0].revID == "1-bb"_sl);
        CHECK(changes[0].sequence == 2_seq);
        CHECK(changes[1].docID == "A"_sl);
        CHECK(changes[1].revID == nullslice);
        CHECK(changes[1].sequence == 0_seq);
    }
}
