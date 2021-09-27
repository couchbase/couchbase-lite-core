//
// c4PerfTest.cc
//
// Copyright 2016-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "fleece/Fleece.h"
#include "c4Test.hh"
#include "c4BlobStore.h"
#include "c4Document+Fleece.h"
#include "c4Query.h"
#include "c4Index.h"
#include "Base.hh"
#include "Benchmark.hh"
#include "FilePath.hh"
#include "SecureRandomize.hh"
#include <fcntl.h>
#include <sys/stat.h>
#include <iostream>
#include <chrono>
#include <thread>
#include <fstream>
#include <cinttypes>
#ifndef _MSC_VER
#include <unistd.h>
#endif

using namespace fleece;
using namespace std;


class PerfTest : public C4Test {
public:
    PerfTest(int variation) :C4Test(variation)
    {
        const char* showFastDir = getenv("CBL_SHOWFAST_DIR");
        if(showFastDir) {
            litecore::FilePath showFastPath(showFastDir, "");
            if(showFastPath.exists() && showFastPath.isDir()) {
                _showFastDir = showFastDir;
            }
        }
    }

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
        Dict::Key typeKey   (FLSTR("Track Type"));
        Dict::Key idKey     (FLSTR("Persistent ID"));
        Dict::Key nameKey   (FLSTR("Name"));
        Dict::Key albumKey  (FLSTR("Album"));
        Dict::Key artistKey (FLSTR("Artist"));
        Dict::Key timeKey   (FLSTR("Total Time"));
        Dict::Key genreKey  (FLSTR("Genre"));
        Dict::Key yearKey   (FLSTR("Year"));
        Dict::Key trackNoKey(FLSTR("Track Number"));
        Dict::Key compKey   (FLSTR("Compilation"));

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
            alloc_slice body = enc.finish(&error);
            REQUIRE(body.buf);
            enc.reset();

            // Save document:
            C4Error c4err;
            C4DocPutRequest rq = {};
            rq.docID = trackID;
            rq.body = (C4Slice)body;
            rq.save = true;
            C4Document *doc = c4doc_put(db, &rq, nullptr, ERROR_INFO(&c4err));
            REQUIRE(doc != nullptr);
            c4doc_release(doc);
            ++numDocs;
        }
        
        return numDocs;
    }


    unsigned queryWhere(const char *whereStr, bool verbose =false) {
        std::vector<std::string> docIDs;
        docIDs.reserve(1200);

        C4Error error;
        C4Query *query = c4query_new2(db, kC4JSONQuery, c4str(whereStr), nullptr, ERROR_INFO(error));
        REQUIRE(query);
        auto e = c4query_run(query, nullptr, kC4SliceNull, ERROR_INFO(error));
        REQUIRE(e);
        while (c4queryenum_next(e, ERROR_INFO(error))) {
            REQUIRE(FLArrayIterator_GetCount(&e->columns) > 0);
            fleece::slice docID = FLValue_AsString(FLArrayIterator_GetValueAt(&e->columns, 0));
            std::string artist = docID.asString();
            if (verbose) std::cerr << artist << "  ";
            docIDs.push_back(artist);
        }
        c4queryenum_release(e);
        c4query_release(query);
        if (verbose) std::cerr << "\n";
        return (unsigned) docIDs.size();
    }


    void readRandomDocs(size_t numDocs, size_t numDocsToRead, const char* sf_title = nullptr) {
        std::cerr << "Reading " <<numDocsToRead<< " random docs...\n";
        Benchmark b;
        for (size_t readNo = 0; readNo < numDocsToRead; ++readNo) {
            char docID[30];
            sprintf(docID, "%07zu", ((unsigned)litecore::RandomNumber() % numDocs) + 1);
            INFO("Reading doc " << docID);
            b.start();
            C4Error error;
            auto doc = c4doc_get(db, c4str(docID), true, ERROR_INFO(error));
            REQUIRE(doc);
            CHECK(c4doc_getProperties(doc) != nullptr);
            c4doc_release(doc);
            b.stop();
        }
        b.printReport(1, "doc");
        if(sf_title) {
            string sf = generateShowfast(b, 1000.0*1000.0, sf_title);
            writeShowFastToFile(sf_title, sf);
        }
    }
    
    
    string generateShowfast(Benchmark &mark, double scale, string title, const char* items = nullptr) {
        if(isEncrypted()) {
            title += "_encrypted";
        }
        
        FLEncoder enc = FLEncoder_NewWithOptions(kFLEncodeJSON, 0, false);
        FLEncoder_BeginArray(enc, 4);
        auto r = mark.range();
        string prefixes[] = { "median_", "mean_", "fast_", "slow_" };
        int p = 0;
        for(auto m : {mark.median(), mark.average(), r.first, r.second}) {
            string fullTitle = prefixes[p++] + title;
            
            FLEncoder_BeginDict(enc, 3);
            FLEncoder_WriteKey(enc, FLSTR("metric"));
            FLEncoder_WriteString(enc, FLStr(fullTitle.c_str()));
            FLEncoder_WriteKey(enc, FLSTR("hidden"));
            FLEncoder_WriteBool(enc, false);
            FLEncoder_WriteKey(enc, FLSTR("value"));
            FLEncoder_WriteDouble(enc, round(m * scale * 1000.0) / 1000.0);
            FLEncoder_EndDict(enc);
        }
        
        FLEncoder_EndArray(enc);
        auto result = FLEncoder_Finish(enc, nullptr);
        auto retVal = string((char *)result.buf, result.size);
        FLSliceResult_Release(result);
        return retVal;
    }
    
    string generateShowfast(double value, string title) {
        if(!_showFastDir) {
            return "";
        }
        
        if(isEncrypted()) {
            title += "_encrypted";
        }
        
        int64_t int_value = (int64_t)value;
        
        FLEncoder enc = FLEncoder_NewWithOptions(kFLEncodeJSON, 0, false);
        FLEncoder_BeginArray(enc, 1);
        FLEncoder_BeginDict(enc, 3);
        FLEncoder_WriteKey(enc, FLSTR("metric"));
        FLEncoder_WriteString(enc, FLStr(title.c_str()));
        FLEncoder_WriteKey(enc, FLSTR("hidden"));
        FLEncoder_WriteBool(enc, false);
        FLEncoder_WriteKey(enc, FLSTR("value"));
        if(int_value == value) {
            FLEncoder_WriteInt(enc, int_value);
        } else {
            FLEncoder_WriteDouble(enc, round(value * 1000.0) / 1000.0);
        }
        
        FLEncoder_EndDict(enc);
        FLEncoder_EndArray(enc);
        
        auto result = FLEncoder_Finish(enc, nullptr);
        auto retVal = string((char *)result.buf, result.size);
        FLSliceResult_Release(result);
        return retVal;
    }
    
    void writeShowFastToFile(string filename, const string& contents) {
        if(!_showFastDir) {
            return;
        }
        
        if(isEncrypted()) {
            filename += "_encrypted";
        }
        
        filename += ".json";
        litecore::FilePath sfPath(_showFastDir, filename);
        
        ofstream fout(sfPath.path(), ios::trunc|ios::out);
        fout.exceptions(ostream::failbit|ostream::badbit);
        fout << contents;
        fout.close();
    }
    
private:
    const char* _showFastDir {nullptr};
};


N_WAY_TEST_CASE_METHOD(PerfTest, "Import iTunesMusicLibrary", "[Perf][C][.slow]") {
    Stopwatch st;
    auto numDocs = importJSONLines(sFixturesDir + "iTunesMusicLibrary.json");
    CHECK(numDocs == 12189);
    st.stop();
    st.printReport("******** Importing JSON w/spaces", numDocs, "doc");
    
    litecore::FilePath path(alloc_slice(c4db_getPath(db)).asString(), "db.sqlite3");
    fprintf(stderr, "******** DB size is %" PRIi64 "\n", path.dataSize());
    reopenDB();
    string sf = generateShowfast((double)numDocs / st.elapsed(), "tunes_import_json");
    writeShowFastToFile("tunes_import_json", sf);
    readRandomDocs(numDocs, 100000, "tunes_read_random_docs");
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
        st.stop();
        st.printReport("SQL query of state", n, "doc");
        const char* sf_title = pass == 0 ?
            "names_sql_query_state" :
            "names_sql_query_state_indexed";
        string sf = generateShowfast(round(double(n) / st.elapsed()), sf_title);
        writeShowFastToFile(sf_title, sf);
        if (complete) CHECK(n == 5053);
        if (pass == 0) {
            Stopwatch st2;
            C4Error error;
			C4Slice property = C4STR("[[\".contact.address.state\"]]");
            REQUIRE(c4db_createIndex(db, C4STR("byState"), property, kC4ValueIndex, nullptr, WITH_ERROR(&error)));
            st2.stop();
            st2.printReport("Creating SQL index of state", 1, "index");
            sf = generateShowfast(round(st2.elapsedMS()), "names_sql_index_creation");
            writeShowFastToFile("names_sql_index_creation", sf);
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
    readRandomDocs(numDocs, 100000, "geoblocks_import_json");
}


N_WAY_TEST_CASE_METHOD(PerfTest, "Import Wikipedia", "[Perf][C][.slow]") {
    // Download https://github.com/diegoceccarelli/json-wikipedia/blob/master/src/test/resources/misc/en-wikipedia-articles-1000-1.json.gz
    // and unzip to C/tests/data/ before running this test.

    auto numDocs = importJSONLines(sFixturesDir + "en-wikipedia-articles-1000-1.json", 15.0, true);
    std:: cerr << "Shared keys:  " << listSharedKeys() << "\n";

    reopenDB();
    readRandomDocs(numDocs, 100000);
}
