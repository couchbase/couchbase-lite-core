//
//  c4PerfTest.cc
//  LiteCore
//
//  Created by Jens Alfke on 9/20/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#include "Fleece.h"     // including this before c4 makes FLSlice and C4Slice compatible
#include "c4Test.hh"
#include "c4Document+Fleece.h"
#include "Base.hh"
#include "Benchmark.hh"
#include <fcntl.h>
#include <assert.h>
#include <sys/stat.h>
#include <iostream>
#include <chrono>
#include <thread>
#ifndef _MSC_VER
#include <unistd.h>
#endif

using namespace fleece;


struct countContext {
    uint64_t count;
    char value[30];
};

// accumulate function that simply totals numeric values. `context` must point to a totalContext.
static void count_accumulate(void *context, C4Key *key, C4Slice value) {
    auto ctx = (countContext*)context;
    ++ctx->count;
}

// reduce function that returns the row count. `context` must point to a countContext.
static C4Slice count_reduce (void *context) {
    auto ctx = (countContext*)context;
    sprintf(ctx->value, "%llu", (unsigned long long)ctx->count);
    ctx->count = 0.0;
    return {ctx->value, strlen(ctx->value)};
}


struct totalContext {
    double total;
    char value[30];
};

// accumulate function that simply totals numeric values. `context` must point to a totalContext.
static void total_accumulate(void *context, C4Key *key, C4Slice value) {
    auto ctx = (totalContext*)context;
    Value v = Value::fromTrustedData(value);
    REQUIRE(v.type() == kFLNumber);
    ctx->total += v.asDouble();
}

// reduce function that returns the row total. `context` must point to a totalContext.
static C4Slice total_reduce (void *context) {
    auto ctx = (totalContext*)context;
    sprintf(ctx->value, "%g", ctx->total);
    ctx->total = 0.0;
    return {ctx->value, strlen(ctx->value)};
}


class PerfTest : public C4Test {
public:
    PerfTest(int variation) :C4Test(variation) { }

    ~PerfTest() {
        c4view_free(artistsView);
        c4view_free(albumsView);
        c4view_free(tracksView);
        c4view_free(likesView);
        c4view_free(statesView);
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

        Encoder enc;
        unsigned numDocs = 0;
        for (Value item : docs) {
            // Check that track is correct type:
            Dict track = item.asDict();

            FLSlice trackType = track.get(typeKey).asString();
            if (trackType != FLSTR("File") && trackType != FLSTR("Remote"))
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


    void indexViews() {
        Dict::Key nameKey   (FLSTR("Name"), true);
        Dict::Key albumKey  (FLSTR("Album"), true);
        Dict::Key artistKey (FLSTR("Artist"), true);
        Dict::Key timeKey   (FLSTR("Total Time"), true);
        Dict::Key trackNoKey(FLSTR("Track Number"), true);
        Dict::Key compKey   (FLSTR("Compilation"), true);

        Encoder enc;
        auto key = c4key_new();

        C4Error error;
        if (!artistsView) {
            artistsView = c4view_open(db, kC4SliceNull, C4STR("Artists"),
                                      C4STR("1"), c4db_getConfig(db), &error);
            REQUIRE(artistsView);
        }
        if (!albumsView) {
            albumsView = c4view_open(db, kC4SliceNull, C4STR("Albums"),
                                     C4STR("1"), c4db_getConfig(db), &error);
            REQUIRE(albumsView);
        }

        C4View* views[2] = {artistsView, albumsView};
        C4Indexer *indexer = c4indexer_begin(db, views, 2, &error);
        REQUIRE(indexer);
        auto e = c4indexer_enumerateDocuments(indexer, &error);
        REQUIRE(e);
        while (c4enum_next(e, &error)) {
            auto doc = c4enum_getDocument(e, &error);
            Dict body = Value::fromTrustedData(doc->selectedRev.body).asDict();
            REQUIRE(body);


            FLSlice artist;
            if (body[compKey].asBool())
                artist = FLSTR("-Compilations-");
            else
                artist   = body[artistKey].asString();
            auto name    = body[nameKey].asString();
            auto album   = body[albumKey].asString();
            auto trackNo = body[trackNoKey].asInt();
            auto time    = body[timeKey];

            // Generate value:
            enc.writeValue(time);
            FLError flError;
            FLSliceResult fval = enc.finish(&flError);
            enc.reset();
            REQUIRE(fval.buf);
            auto value = (C4Slice)fval;

            // Emit to artists view:
            unsigned nKeys = 0;
            if (artist.buf && name.buf) {
                nKeys = 1;
                // Generate key:
                c4key_beginArray(key);
                c4key_addString(key, artist);
                if (album.buf)
                    c4key_addString(key, album);
                else
                    c4key_addNull(key);
                c4key_addNumber(key, trackNo);
                c4key_addString(key, name);
                c4key_addNumber(key, 1);
                c4key_endArray(key);
            }
            REQUIRE(c4indexer_emit(indexer, doc, 0, nKeys, &key, &value, &error));
            c4key_reset(key);

            // Emit to albums view:
            nKeys = 0;
            if (album.buf) {
                nKeys = 1;
                // Generate key:
                c4key_beginArray(key);
                c4key_addString(key, album);
                if (artist.buf)
                    c4key_addString(key, artist);
                else
                    c4key_addNull(key);
                c4key_addNumber(key, trackNo);
                if (!name.buf)
                    name = FLSTR("");
                c4key_addString(key, name);
                c4key_addNumber(key, 1);
                c4key_endArray(key);
            }
            REQUIRE(c4indexer_emit(indexer, doc, 1, nKeys, &key, &value, &error));
            c4key_reset(key);

            FLSliceResult_Free(fval);
            c4doc_free(doc);
        }
        c4enum_free(e);
        REQUIRE(error.code == 0);

        REQUIRE(c4indexer_end(indexer, true, &error));
        c4key_free(key);
    }


    void indexTracksView() {
        Dict::Key nameKey(FLSTR("Name"), true);
        auto key = c4key_new();

        C4Error error;
        if (!tracksView) {
            tracksView = c4view_open(db, kC4SliceNull, C4STR("Tracks"),
                                     C4STR("1"), c4db_getConfig(db), &error);
            REQUIRE(tracksView);
        }
        C4View* views[1] = {tracksView};
        C4Indexer *indexer = c4indexer_begin(db, views, 1, &error);
        REQUIRE(indexer);
        auto e = c4indexer_enumerateDocuments(indexer, &error);
        REQUIRE(e);
        while (c4enum_next(e, &error)) {
            auto doc = c4enum_getDocument(e, &error);
            Dict body = FLValue_AsDict( FLValue_FromTrustedData(doc->selectedRev.body) );
            REQUIRE(body);
            auto name    = body[nameKey].asString();

            c4key_reset(key);
            c4key_addString(key, name);

            C4Slice value = kC4SliceNull;
            REQUIRE(c4indexer_emit(indexer, doc, 0, 1, &key, &value, &error));
            c4key_reset(key);

            c4doc_free(doc);
        }
        c4enum_free(e);
        REQUIRE(error.code == 0);

        REQUIRE(c4indexer_end(indexer, true, &error));
        c4key_free(key);
    }


    unsigned indexLikesView() {
        Dict::Key likesKey(FLSTR("likes"),
                                                            c4db_getFLSharedKeys(db));
        C4Key *keys[3] = {c4key_new(), c4key_new(), c4key_new()};
        C4Slice values[3] = {};
        unsigned totalLikes = 0;

        C4Error error;
        if (!likesView) {
            likesView = c4view_open(db, kC4SliceNull, C4STR("likes"),
                                    C4STR("1"), c4db_getConfig(db), &error);
            REQUIRE(likesView);
        }
        C4View* views[1] = {likesView};
        C4Indexer *indexer = c4indexer_begin(db, views, 1, &error);
        REQUIRE(indexer);
        auto e = c4indexer_enumerateDocuments(indexer, &error);
        REQUIRE(e);
        while (c4enum_next(e, &error)) {
            auto doc = c4enum_getDocument(e, &error);
            Dict body = FLValue_AsDict( FLValue_FromTrustedData(doc->selectedRev.body) );
            REQUIRE(body);
            auto likes = body[likesKey].asArray();

            Array::iterator iter(likes);
            unsigned nLikes;
            for (nLikes = 0; nLikes < 3; ++nLikes) {
                FLSlice like = iter->asString();
                if (!like.buf)
                    break;
                c4key_reset(keys[nLikes]);
                c4key_addString(keys[nLikes], like);
                ++iter;
            }
            totalLikes += nLikes;

            REQUIRE(c4indexer_emit(indexer, doc, 0, nLikes, keys, values, &error));

            c4doc_free(doc);
        }
        c4enum_free(e);
        REQUIRE(error.code == 0);

        REQUIRE(c4indexer_end(indexer, true, &error));

        for (unsigned i = 0; i < 3; i++)
            c4key_free(keys[i]);
        return totalLikes;
    }


    unsigned indexStatesView() {
        auto sk = c4db_getFLSharedKeys(db);
        Dict::Key contactKey(FLSTR("contact"), sk);
        Dict::Key addressKey(FLSTR("address"), sk);
        Dict::Key stateKey  (FLSTR("state"), sk);
        C4Key *key = c4key_new();
        C4Slice values[3] = {};
        unsigned totalStates = 0;

        C4Error error;
        if (!statesView) {
            statesView = c4view_open(db, kC4SliceNull, C4STR("states"),
                                    C4STR("1"), c4db_getConfig(db), &error);
            REQUIRE(statesView);
        }
        C4View* views[1] = {statesView};
        C4Indexer *indexer = c4indexer_begin(db, views, 1, &error);
        REQUIRE(indexer);
        auto e = c4indexer_enumerateDocuments(indexer, &error);
        REQUIRE(e);
        while (c4enum_next(e, &error)) {
            auto doc = c4enum_getDocument(e, &error);
            Dict body = FLValue_AsDict( FLValue_FromTrustedData(doc->selectedRev.body) );
            REQUIRE(body);
            auto contact = body[contactKey].asDict();
            auto address = contact[addressKey].asDict();
            auto state = address[stateKey].asString();

            unsigned nStates = 0;
            if (state.buf) {
                c4key_reset(key);
                c4key_addString(key, state);
                nStates = 1;
                totalStates++;
            }
            C4Slice value = kC4SliceNull;

            REQUIRE(c4indexer_emit(indexer, doc, 0, nStates, &key, &value, &error));

            c4doc_free(doc);
        }
        c4enum_free(e);
        REQUIRE(error.code == 0);

        REQUIRE(c4indexer_end(indexer, true, &error));

        c4key_free(key);
        return totalStates;
    }


    unsigned queryGrouped(C4View *view, C4ReduceFunction reduce, bool verbose =false) {
        C4QueryOptions options = kC4DefaultQueryOptions;
        options.reduce = &reduce;
        options.groupLevel = 1;
        return runQuery(view, options, verbose);
    }

    unsigned runQuery(C4View *view, C4QueryOptions &options, bool verbose =false) {
        std::vector<std::string> allKeys;
        allKeys.reserve(1200);
        C4Error error;
        auto query = c4view_query(view, &options, &error);
        C4SliceResult keySlice;
        while (c4queryenum_next(query, &error)) {
            C4KeyReader key = query->key;
            if (c4key_peek(&key) == kC4Array) {
                c4key_skipToken(&key);
                keySlice = c4key_readString(&key);
            } else {
                REQUIRE(c4key_peek(&key) == kC4String);
                keySlice = c4key_readString(&key);
            }
            REQUIRE(keySlice.buf);
            std::string keyStr((const char*)keySlice.buf, keySlice.size);
            if (verbose) std::cerr << keyStr << " (" << std::string((char*)query->value.buf, query->value.size) << ")  ";
            allKeys.push_back(keyStr);
            c4slice_free(keySlice);
        }
        c4queryenum_free(query);
        if (verbose) std::cerr << "\n";
        return (unsigned) allKeys.size();
    }


    unsigned queryWhere(const char *whereStr, bool verbose =false) {
        std::vector<std::string> docIDs;
        docIDs.reserve(1200);

        C4Error error;
        C4Query *query = c4query_new(db, c4str(whereStr), &error);
        REQUIRE(query);
        auto e = c4query_run(query, nullptr, kC4SliceNull, &error);
        C4SliceResult artistSlice;
        while (c4queryenum_next(e, &error)) {
            std::string artist((const char*)e->docID.buf, e->docID.size);
            if (verbose) std::cerr << artist << "  ";
            docIDs.push_back(artist);
        }
        c4queryenum_free(e);
        c4query_free(query);
        if (verbose) std::cerr << "\n";
        return (unsigned) docIDs.size();
    }


    C4View *artistsView {nullptr};
    C4View *albumsView {nullptr};
    C4View *tracksView {nullptr};
    C4View *likesView {nullptr};
    C4View *statesView {nullptr};
};


N_WAY_TEST_CASE_METHOD(PerfTest, "Performance", "[Perf][C]") {
    auto jsonData = readFile(sFixturesDir + "iTunesMusicLibrary.json");
    FLError error;
    FLSliceResult fleeceData = FLData_ConvertJSON({jsonData.buf, jsonData.size}, &error);
    free((void*)jsonData.buf);
    Array root = FLValue_AsArray(FLValue_FromTrustedData((C4Slice)fleeceData));
    unsigned numDocs;

    {
        Stopwatch st;
        numDocs = insertDocs(root);
        CHECK(numDocs == 12189);
        st.printReport("Writing docs", numDocs, "doc");
    }
    FLSliceResult_Free(fleeceData);
    {
        Stopwatch st;
        indexViews();
        st.printReport("Indexing Artist/Album views", numDocs, "doc");
    }
    {
        Stopwatch st;
        indexTracksView();
        st.printReport("Indexing Tracks view", numDocs, "doc");
    }
    {
        Stopwatch st;
        totalContext context = {};
        auto numArtists = queryGrouped(artistsView, {total_accumulate, total_reduce, &context});
        CHECK(numArtists == 1141);
        st.printReport("Grouped query of Artist view", numDocs, "doc");
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
    {
        Stopwatch st;
        auto readNo = 0;
        for (; readNo < 100000; ++readNo) {
            char docID[30];
            sprintf(docID, "%07u", ((unsigned)random() % numDocs) + 1);
            C4Error error;
            auto doc = c4doc_get(db, c4str(docID), true, &error);
            REQUIRE(doc);
            REQUIRE(doc->selectedRev.body.size > 10);
            c4doc_free(doc);
        }
        st.printReport("Reading random docs", readNo, "doc");
    }
	std::this_thread::sleep_for(std::chrono::seconds(1)); //TEMP
}

N_WAY_TEST_CASE_METHOD(PerfTest, "Import names", "[Perf][C][.slow]") {
    // Download https://github.com/arangodb/example-datasets/raw/master/RandomUsers/names_300000.json
    // to C/tests/data/ before running this test.
    //
    // Docs look like:
    // {"name":{"first":"Travis","last":"Mutchler"},"gender":"female","birthday":"1990-12-21","contact":{"address":{"street":"22 Kansas Cir","zip":"45384","city":"Wilberforce","state":"OH"},"email":["Travis.Mutchler@nosql-matters.org","Travis@nosql-matters.org"],"region":"937","phone":["937-3512486"]},"likes":["travelling"],"memberSince":"2010-01-01"}

    auto numDocs = importJSONLines(sFixturesDir + "names_300000.json", 15.0, true);
    const bool complete = (numDocs == 300000);
#ifdef NDEBUG
    REQUIRE(numDocs == 300000);
#endif
    {
        Stopwatch st;
        auto totalLikes = indexLikesView();
        C4Log("Total of %u likes", totalLikes);
        st.printReport("Indexing Likes view", numDocs, "doc");
        if (complete) CHECK(totalLikes == 345986);
    }
    {
        Stopwatch st;
        countContext context = {};
        auto numLikes = queryGrouped(likesView, {count_accumulate, count_reduce, &context}, true);
        st.printReport("Querying all likes", numLikes, "like");
        if (complete) CHECK(numLikes == 15);
    }
    {
        Stopwatch st;
        auto total = indexStatesView();
        st.printReport("Indexing States view", numDocs, "doc");
        if (complete) CHECK(total == 300000);
    }
    {
        C4QueryOptions options = kC4DefaultQueryOptions;
        C4Key *key = c4key_new();
        c4key_addString(key, C4STR("WA"));
        options.startKey = options.endKey = key;
        Stopwatch st;
        auto total = runQuery(statesView, options);
        c4key_free(key);
        st.printReport("Querying States view", total, "row");
        if (complete) CHECK(total == 5053);
    }
    if (isSQLite() && !isRevTrees()) {
        for (int pass = 0; pass < 2; ++pass) {
            Stopwatch st;
            auto n = queryWhere("{\"contact.address.state\": \"WA\"}");
            st.printReport("SQL query of state", n, "doc");
            if (complete) CHECK(n == 5053);
            if (pass == 0) {
                Stopwatch st2;
                C4Error error;
                REQUIRE(c4db_createIndex(db, C4STR("contact.address.state"), kC4ValueIndex, nullptr, &error));
                st2.printReport("Creating SQL index of state", 1, "index");
            }
        }
    }
}
