//
// c4PerfTest.cc
//
// Copyright (c) 2016 Couchbase, Inc All rights reserved.
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

#include "Fleece.h"     // including this before c4 makes FLSlice and C4Slice compatible
#include "c4Test.hh"
#include "c4Document+Fleece.h"
#include "Base.hh"
#include "Benchmark.hh"
#include <fcntl.h>
#include <sys/stat.h>
#include <iostream>
#include <chrono>
#include <thread>
#ifndef _MSC_VER
#include <unistd.h>
#endif

#ifndef __APPLE__
#include "arc4random.h"
#endif

using namespace fleece;


class PerfTest : public C4Test {
public:
    PerfTest(int variation) :C4Test(variation) { }

    // Copies a Fleece dictionary key/value to an encoder
    static bool copyValue(Dict srcDict, Dict::Key &key, Encoder &enc) {
        Value value = srcDict[key];
        if (!value)
            return false;
        enc.writeKey(key);
        enc.writeValue(value);
        return true;
    }


    unsigned insertDocs(Array docs) {
        Dict::Key typeKey   (FLSTR("Track Type"), true);
        Dict::Key idKey     (FLSTR("Persistent ID"), true);
        Dict::Key nameKey   (FLSTR("Name"), true);
        Dict::Key albumKey  (FLSTR("Album"), true);
        Dict::Key artistKey (FLSTR("Artist"), true);
        Dict::Key timeKey   (FLSTR("Total Time"), true);
        Dict::Key genreKey  (FLSTR("Genre"), true);
        Dict::Key yearKey   (FLSTR("Year"), true);
        Dict::Key trackNoKey(FLSTR("Track Number"), true);
        Dict::Key compKey   (FLSTR("Compilation"), true);

        TransactionHelper t(db);

        Encoder enc(c4db_createFleeceEncoder(db));
        unsigned numDocs = 0;
        for (Value item : docs) {
            // Check that track is correct type:
            Dict track = item.asDict();

            FLSlice trackType = track.get(typeKey).asString();
			FLSlice file = FLSTR("File");
			FLSlice remote = FLSTR("Remote");
            if (trackType != file && trackType != remote)
                continue;

            FLSlice trackID = track.get(idKey).asString();
            REQUIRE(trackID.buf);

            // Encode doc body:
            enc.beginDict();
            REQUIRE(copyValue(track, nameKey, enc));
            copyValue(track, albumKey, enc);
            copyValue(track, artistKey, enc);
            copyValue(track, timeKey, enc);
            copyValue(track, genreKey, enc);
            copyValue(track, yearKey, enc);
            copyValue(track, trackNoKey, enc);
            copyValue(track, compKey, enc);
            enc.endDict();
            FLError error;
            FLSliceResult body = enc.finish(&error);
            REQUIRE(body.buf);
            enc.reset();

            // Save document:
            C4Error c4err;
            C4DocPutRequest rq = {};
            rq.docID = trackID;
            rq.body = (C4Slice)body;
            rq.save = true;
            C4Document *doc = c4doc_put(db, &rq, nullptr, &c4err);
            REQUIRE(doc != nullptr);
            c4doc_free(doc);
            FLSliceResult_Free(body);
            ++numDocs;
        }
        
        return numDocs;
    }


    unsigned queryWhere(const char *whereStr, bool verbose =false) {
        std::vector<std::string> docIDs;
        docIDs.reserve(1200);

        C4Error error;
        C4Query *query = c4query_new(db, c4str(whereStr), &error);
        REQUIRE(query);
        auto e = c4query_run(query, nullptr, kC4SliceNull, &error);
        while (c4queryenum_next(e, &error)) {
            REQUIRE(FLArrayIterator_GetCount(&e->columns) > 0);
            fleece::slice docID = FLValue_AsString(FLArrayIterator_GetValueAt(&e->columns, 0));
            std::string artist = docID.asString();
            if (verbose) std::cerr << artist << "  ";
            docIDs.push_back(artist);
        }
        c4queryenum_free(e);
        c4query_free(query);
        if (verbose) std::cerr << "\n";
        return (unsigned) docIDs.size();
    }


    void readRandomDocs(size_t numDocs, size_t numDocsToRead) {
        std::cerr << "Reading " <<numDocsToRead<< " random docs...\n";
        Benchmark b;
        for (size_t readNo = 0; readNo < numDocsToRead; ++readNo) {
            char docID[30];
            sprintf(docID, "%07zu", ((unsigned)arc4random() % numDocs) + 1);
            INFO("Reading doc " << docID);
            b.start();
            C4Error error;
            auto doc = c4doc_get(db, c4str(docID), true, &error);
            REQUIRE(doc);
            REQUIRE(doc->selectedRev.body.size > 30);
            c4doc_free(doc);
            b.stop();
        }
        b.printReport(1, "doc");
    }
};


N_WAY_TEST_CASE_METHOD(PerfTest, "Performance", "[Perf][C]") {
    Stopwatch st;
    auto numDocs = importJSONLines(sFixturesDir + "iTunesMusicLibrary.json");
    CHECK(numDocs == 12189);
}


N_WAY_TEST_CASE_METHOD(PerfTest, "Import names", "[Perf][C][.slow]") {
    // Download https://github.com/arangodb/example-datasets/raw/master/RandomUsers/names_300000.json
    // to C/tests/data/ before running this test.
    //
    // Docs look like:
    // {"name":{"first":"Travis","last":"Mutchler"},"gender":"female","birthday":"1990-12-21","contact":{"address":{"street":"22 Kansas Cir","zip":"45384","city":"Wilberforce","state":"OH"},"email":["Travis.Mutchler@nosql-matters.org","Travis@nosql-matters.org"],"region":"937","phone":["937-3512486"]},"likes":["travelling"],"memberSince":"2010-01-01"}

    auto numDocs = importJSONLines(sFixturesDir + "names_300000.json", 30.0, true);
    const bool complete = (numDocs == 300000);
#ifdef NDEBUG
    REQUIRE(numDocs == 300000);
#endif
    std:: cerr << "Shared keys:  " << listSharedKeys() << "\n";
    for (int pass = 0; pass < 2; ++pass) {
        Stopwatch st;
        auto n = queryWhere("[\"=\", [\".contact.address.state\"], \"WA\"]");
        st.printReport("SQL query of state", n, "doc");
        if (complete) CHECK(n == 5053);
        if (pass == 0) {
            Stopwatch st2;
            C4Error error;
			C4Slice property = C4STR("[[\".contact.address.state\"]]");
            REQUIRE(c4db_createIndex(db, C4STR("byState"), property, kC4ValueIndex, nullptr, &error));
            st2.printReport("Creating SQL index of state", 1, "index");
        }
    }
}


N_WAY_TEST_CASE_METHOD(PerfTest, "Import geoblocks", "[Perf][C][.slow]") {
    // Download https://github.com/arangodb/example-datasets/raw/master/IPRanges/geoblocks.json
    // to C/tests/data/ before running this test.
    //
    // Docs look like:
    // { "locId" : 17, "endIpNum" : 16777471, "startIpNum" : 16777216, "geo" : [ -27, 133 ] }

    auto numDocs = importJSONLines(sFixturesDir + "geoblocks.json", 15.0, true);
    reopenDB();
    readRandomDocs(numDocs, 100000);
}


N_WAY_TEST_CASE_METHOD(PerfTest, "Import Wikipedia", "[Perf][C][.slow]") {
    // Download https://github.com/diegoceccarelli/json-wikipedia/blob/master/src/test/resources/misc/en-wikipedia-articles-1000-1.json.gz
    // and unzip to C/tests/data/ before running this test.

    auto numDocs = importJSONLines(sFixturesDir + "en-wikipedia-articles-1000-1.json", 15.0, true);
    std:: cerr << "Shared keys:  " << listSharedKeys() << "\n";

    reopenDB();
    readRandomDocs(numDocs, 100000);
}
