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


namespace litecore {

    class SequenceTrackerTest {     // SequenceTracker declares this class a friend
    public:
        SequenceTracker tracker;
        sequence_t seq = 0;

        // These methods provide access to private members of SequenceTracker

        string dump() { return dump(tracker); }

        static string dump(SequenceTracker &tracker) {
            stringstream s;
            s << "[";
            bool first = true;
            for (auto i = tracker.begin(); i != tracker.end(); ++i) {
                if (first)
                    first = false;
                else
                    s << ", ";
                if (i->isPlaceholder())
                    s << "*";
                else
                    s << (string)i->docID << "@" << i->sequence;
            }
            s << "]";
            return s.str();
        }

        SequenceTracker::const_iterator since(sequence_t s) {
            return tracker._since(s);
        }
        
        SequenceTracker::const_iterator end() {
            return tracker.end();
        }
        
    };
    
}


TEST_CASE_METHOD(litecore::SequenceTrackerTest, "SequenceTracker", "[notification]") {
    DatabaseChangeNotifier cn(tracker, nullptr);   // Without one the tracker won't bother remembering docs!
    tracker.documentChanged("A"_sl, ++seq);
    tracker.documentChanged("B"_sl, ++seq);
    tracker.documentChanged("C"_sl, ++seq);
    REQUIRE(dump() == "[*, A@1, B@2, C@3]");
    CHECK(tracker.lastSequence() == seq);
    tracker.documentChanged("B"_sl, ++seq);
    REQUIRE(dump() == "[*, A@1, C@3, B@4]");
    tracker.documentChanged("B"_sl, ++seq);
    CHECK(tracker.lastSequence() == seq);
    REQUIRE(dump() == "[*, A@1, C@3, B@5]");
    tracker.documentChanged("A"_sl, ++seq);
    CHECK(tracker.lastSequence() == seq);
    REQUIRE(dump() == "[*, C@3, B@5, A@6]");
    tracker.documentChanged("D"_sl, ++seq);
    CHECK(tracker.lastSequence() == seq);
    REQUIRE(dump() == "[*, C@3, B@5, A@6, D@7]");

    REQUIRE(since(0)->docID == "C"_sl);
    REQUIRE(since(4)->docID == "B"_sl);
    REQUIRE(since(5)->docID == "A"_sl);
    REQUIRE(since(6)->docID == "D"_sl);
    REQUIRE(since(7) == end());
}


TEST_CASE_METHOD(litecore::SequenceTrackerTest, "SequenceTracker DatabaseChangeNotifier", "[notification]") {
    DatabaseChangeNotifier cn(tracker, nullptr);   // Without one the tracker won't bother remembering docs!
    tracker.documentChanged("A"_sl, ++seq);
    tracker.documentChanged("B"_sl, ++seq);
    tracker.documentChanged("C"_sl, ++seq);

    int count1=0, count2=0, count3=0;
    DatabaseChangeNotifier cn1(tracker, [&](DatabaseChangeNotifier&) {++count1;});
    DatabaseChangeNotifier cn2(tracker, [&](DatabaseChangeNotifier&) {++count2;});
    {
        DatabaseChangeNotifier cn3(tracker, [&](DatabaseChangeNotifier&) {++count3;}, 1);
        REQUIRE(dump() == "[*, A@1, *, B@2, C@3, *, *]");

        slice changes[5];
        REQUIRE(cn3.readChanges(changes, 5) == 2);
        CHECK(changes[0] == "B"_sl);
        CHECK(changes[1] == "C"_sl);
        REQUIRE(dump() == "[*, A@1, B@2, C@3, *, *, *]");
        REQUIRE(!cn3.hasChanges());
        REQUIRE(cn3.readChanges(changes, 5) == 0);

        CHECK(count1==0);
        CHECK(count2==0);
        CHECK(count3==0);

        tracker.documentChanged("B"_sl, ++seq);

        REQUIRE(cn1.readChanges(changes, 5) == 1);
        CHECK(changes[0] == "B"_sl);
        REQUIRE(cn1.readChanges(changes, 5) == 0);

        CHECK(count1==1);
        CHECK(count2==1);
        CHECK(count3==1);

        tracker.documentChanged("C"_sl, ++seq);

        CHECK(count1==2);   // was notified again because it called changes() after 1st change
        CHECK(count2==1);   // wasn't because it didn't
        CHECK(count3==1);   // ditto
        REQUIRE(dump() == "[*, A@1, *, *, B@4, *, C@5]");
    }
    // After cn3 is destructed:
    REQUIRE(dump() == "[*, A@1, *, B@4, *, C@5]");
}


TEST_CASE_METHOD(litecore::SequenceTrackerTest, "SequenceTracker DocChangeNotifier", "[notification]") {
    std::unique_ptr<DatabaseChangeNotifier> cn;

    SECTION("With db change notifier") {
        cn.reset(new DatabaseChangeNotifier(tracker, nullptr));
    }
    SECTION("Without db change notifier") {
        // don't initialize cn. Now the tracker isn't recording document changes...
    }

    tracker.documentChanged("A"_sl, ++seq);
    tracker.documentChanged("B"_sl, ++seq);
    tracker.documentChanged("C"_sl, ++seq);

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

    tracker.documentChanged("A"_sl, ++seq);
    CHECK(countA==1);
    CHECK(countB==0);

    tracker.documentChanged("B"_sl, ++seq);
    CHECK(countA==1);
    CHECK(countB==1);

    {
        DocChangeNotifier cnB2(tracker,"B"_sl, [&](DocChangeNotifier&,slice,sequence_t) {++countB2;});
        tracker.documentChanged("B"_sl, ++seq);
        CHECK(countA==1);
        CHECK(countB==2);
        CHECK(countB2==1);
    }

    tracker.documentChanged("B"_sl, ++seq);
    CHECK(countA==1);
    CHECK(countB==3);
    CHECK(countB2==1);
}


TEST_CASE("SequenceTracker Transaction", "[notification]") {
    std::vector<const SequenceTracker::Entry*> gotChanges;
    SequenceTracker tracker([&](SequenceTracker& t, const std::vector<const SequenceTracker::Entry*>& changes) {
        gotChanges = changes;
    });
    DatabaseChangeNotifier cn(tracker, nullptr);   // Without one the tracker won't bother remembering docs!

    sequence_t seq = 0;
    tracker.documentChanged("A"_sl, ++seq);
    tracker.documentChanged("B"_sl, ++seq);
    tracker.documentChanged("C"_sl, ++seq);
    CHECK(SequenceTrackerTest::dump(tracker) == "[*, A@1, B@2, C@3]");

    tracker.beginTransaction();
    tracker.documentChanged("B"_sl, ++seq);
    tracker.documentChanged("D"_sl, ++seq);

    CHECK(SequenceTrackerTest::dump(tracker) == "[*, A@1, C@3, *, B@4, D@5]");

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

    SECTION("Commit") {
        tracker.endTransaction(true);
        CHECK(tracker.lastSequence() == 5);

        CHECK(SequenceTrackerTest::dump(tracker) == "[*, A@1, C@3, B@4, D@5]");

        REQUIRE(gotChanges.size() == 2);
        CHECK(gotChanges[0]->docID == "B"_sl);
        CHECK(gotChanges[0]->sequence == 4);
        CHECK(gotChanges[1]->docID == "D"_sl);
        CHECK(gotChanges[1]->sequence == 5);

        CHECK(countA == 0);
        CHECK(countB == 0);
        CHECK(countD == 0);
    }

    SECTION("Abort") {
        tracker.endTransaction(false);
        CHECK(tracker.lastSequence() == 3);
        CHECK(SequenceTrackerTest::dump(tracker) == "[*, A@1, C@3, B@2, D@0]");

        REQUIRE(gotChanges.size() == 0);

        CHECK(countA == 0);
        CHECK(countB == 1);
        CHECK(countD == 1);
    }
}
