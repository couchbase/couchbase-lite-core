//
//  SequenceTrackerTest.cc
//  LiteCore
//
//  Created by Jens Alfke on 11/1/16.
//  Copyright © 2016 Couchbase. All rights reserved.
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

    class SequenceTrackerTest {     // SequenceTracker declares this class a friend
    public:
        SequenceTracker tracker;
        sequence_t seq = 0;

        // These methods provide access to private members of SequenceTracker

#if DEBUG
        string dump() { return tracker.dump(); }
#endif

        SequenceTracker::const_iterator since(sequence_t s) {
            return tracker._since(s);
        }
        
        SequenceTracker::const_iterator end() {
            return tracker.end();
        }
        
    };
    
}


TEST_CASE_METHOD(litecore::SequenceTrackerTest, "SequenceTracker", "[notification]") {
    tracker.beginTransaction();
    tracker.documentChanged("A"_asl, "1-aa"_asl, ++seq);
    tracker.documentChanged("B"_asl, "1-bb"_asl, ++seq);
    tracker.documentChanged("C"_asl, "1-cc"_asl, ++seq);
    REQUIRE_IF_DEBUG(dump() == "[(A@1, B@2, C@3)]");
    CHECK(tracker.lastSequence() == seq);
    tracker.documentChanged("B"_asl, "2-bb"_asl, ++seq);
    REQUIRE_IF_DEBUG(dump() == "[(A@1, C@3, B@4)]");
    tracker.documentChanged("B"_asl, "3-bb"_asl, ++seq);
    CHECK(tracker.lastSequence() == seq);
    REQUIRE_IF_DEBUG(dump() == "[(A@1, C@3, B@5)]");
    tracker.documentChanged("A"_asl, "2-aa"_asl, ++seq);
    CHECK(tracker.lastSequence() == seq);
    REQUIRE_IF_DEBUG(dump() == "[(C@3, B@5, A@6)]");
    tracker.documentChanged("D"_asl, "1-dd"_asl, ++seq);
    CHECK(tracker.lastSequence() == seq);
    REQUIRE_IF_DEBUG(dump() == "[(C@3, B@5, A@6, D@7)]");

    REQUIRE(since(0)->docID == "C"_sl);
    REQUIRE(since(4)->docID == "B"_sl);
    REQUIRE(since(5)->docID == "A"_sl);
    REQUIRE(since(6)->docID == "D"_sl);
    REQUIRE(since(7) == end());
}


TEST_CASE_METHOD(litecore::SequenceTrackerTest, "SequenceTracker DatabaseChangeNotifier", "[notification]") {
    tracker.beginTransaction();
    tracker.documentChanged("A"_asl, "1-aa"_asl, ++seq);
    tracker.documentChanged("B"_asl, "1-bb"_asl, ++seq);
    tracker.documentChanged("C"_asl, "1-cc"_asl, ++seq);

    int count1=0, count2=0, count3=0;
    DatabaseChangeNotifier cn1(tracker, [&](DatabaseChangeNotifier&) {++count1;});
    DatabaseChangeNotifier cn2(tracker, [&](DatabaseChangeNotifier&) {++count2;});
    {
        DatabaseChangeNotifier cn3(tracker, [&](DatabaseChangeNotifier&) {++count3;}, 1);
        REQUIRE_IF_DEBUG(dump() == "[(A@1, *, B@2, C@3, *, *)]");

        SequenceTracker::Change changes[5];
        bool external;
        REQUIRE(cn3.readChanges(changes, 5, external) == 2);
        CHECK(!external);
        CHECK(changes[0].docID == "B"_sl);
        CHECK(changes[0].revID == "1-bb"_sl);
        CHECK(changes[0].sequence == 2);
        CHECK(changes[1].docID == "C"_sl);
        REQUIRE_IF_DEBUG(dump() == "[(A@1, B@2, C@3, *, *, *)]");
        REQUIRE(!cn3.hasChanges());
        REQUIRE(cn3.readChanges(changes, 5, external) == 0);

        CHECK(count1==0);
        CHECK(count2==0);
        CHECK(count3==0);

        tracker.documentChanged("B"_asl, "2-bb"_asl, ++seq);

        REQUIRE(cn1.readChanges(changes, 5, external) == 1);
        CHECK(changes[0].docID == "B"_sl);
        CHECK(changes[0].revID == "2-bb"_sl);
        CHECK(changes[0].sequence == 4);
        CHECK(!external);
        REQUIRE(cn1.readChanges(changes, 5, external) == 0);
        CHECK(!external);

        CHECK(count1==1);
        CHECK(count2==1);
        CHECK(count3==1);

        tracker.documentChanged("C"_asl, "2-cc"_asl, ++seq);

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
    
    std::unique_ptr<DatabaseChangeNotifier> cn;

    SECTION("With db change notifier") {
        cn = make_unique<DatabaseChangeNotifier>(tracker, nullptr);
    }
    SECTION("Without db change notifier") {
        // don't initialize cn. Now the tracker isn't recording document changes...
    }

    tracker.documentChanged("A"_asl, "1-aa"_asl, ++seq);
    tracker.documentChanged("B"_asl, "1-bb"_asl, ++seq);
    tracker.documentChanged("C"_asl, "1-cc"_asl, ++seq);

    int countA=0, countB=0, countB2=0;

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

    tracker.documentChanged("A"_asl, "2-aa"_asl, ++seq);
    CHECK(countA==1);
    CHECK(countB==0);

    tracker.documentChanged("B"_asl, "2-bb"_asl, ++seq);
    CHECK(countA==1);
    CHECK(countB==1);

    {
        DocChangeNotifier cnB2(tracker,"B"_sl, [&](DocChangeNotifier&,slice,sequence_t) {++countB2;});
        tracker.documentChanged("B"_asl, "3-bb"_asl, ++seq);
        CHECK(countA==1);
        CHECK(countB==2);
        CHECK(countB2==1);
    }

    tracker.documentChanged("B"_asl, "4-bb"_asl, ++seq);
    CHECK(countA==1);
    CHECK(countB==3);
    CHECK(countB2==1);
}


TEST_CASE("SequenceTracker Transaction", "[notification]") {
    SequenceTracker tracker;

    SequenceTracker::Change changes[10];
    size_t numChanges;
    bool external;
    DatabaseChangeNotifier cn(tracker, nullptr);

    // First create some docs:
    sequence_t seq = 0;
    tracker.beginTransaction();
    tracker.documentChanged("A"_asl, "1-aa"_asl, ++seq);
    tracker.documentChanged("B"_asl, "1-bb"_asl, ++seq);
    tracker.documentChanged("C"_asl, "1-cc"_asl, ++seq);
    tracker.endTransaction(true);
    CHECK_IF_DEBUG(tracker.dump() == "[*, A@1, B@2, C@3]");
    numChanges = cn.readChanges(changes, 10, external);
    REQUIRE(numChanges == 3);

    // Now start a transaction and make two more changes:
    tracker.beginTransaction();
    tracker.documentChanged("B"_asl, "2-bb"_asl, ++seq);
    tracker.documentChanged("D"_asl, "1-dd"_asl, ++seq);

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
        CHECK(tracker.lastSequence() == 5);

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
        CHECK(tracker.lastSequence() == 5);

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
        CHECK(tracker.lastSequence() == 3);
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
        CHECK(tracker.lastSequence() == 3);
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
#define FOO(x) 


TEST_CASE_METHOD(litecore::SequenceTrackerTest, "SequenceTracker ExternalChanges", "[notification]") {
    // Add some docs:
    tracker.beginTransaction();
    tracker.documentChanged("A"_asl, "1-aa"_asl, ++seq);
    tracker.documentChanged("B"_asl, "1-bb"_asl, ++seq);
    tracker.documentChanged("C"_asl, "1-cc"_asl, ++seq);
    tracker.endTransaction(true);

    SequenceTracker track2;
    track2.beginTransaction();
    track2.documentChanged("B"_asl, "2-bb"_asl, ++seq);
    track2.documentChanged("Z"_asl, "1-ff"_asl, ++seq);

    // Notify tracker about the transaction from track2:
    tracker.addExternalTransaction(track2);
    track2.endTransaction(true);

    CHECK_IF_DEBUG(tracker.dump() == "[A@1, C@3, B@4', Z@5']");

    DatabaseChangeNotifier cn(tracker, nullptr, 0);
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
