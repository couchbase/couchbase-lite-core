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
#include <unistd.h>
#include <iostream>

// Download from https://github.com/arangodb/example-datasets and update this path accordingly:
#define kLargeDataSetsDir "/Couchbase/example-datasets-master/"

using namespace fleece;


static const char* kJSONFilePath = "C/tests/iTunesMusicLibrary.json";



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
    sprintf(ctx->value, "%llu", ctx->count);
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
    FLValue v = FLValue_FromTrustedData(value);
    REQUIRE(FLValue_GetType(v) == kFLNumber);
    ctx->total += FLValue_AsDouble(v);
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
    static bool copyValue(FLDict srcDict, FLDictKey *key, FLEncoder enc) {
        FLValue value = FLDict_GetWithKey(srcDict, key);
        if (!value)
            return false;
        FLEncoder_WriteKey(enc, FLDictKey_GetString(key));
        FLEncoder_WriteValue(enc, value);
        return true;
    }


    unsigned insertDocs(FLArray docs) {
        FLDictKey typeKey   = FLDictKey_Init(FLSTR("Track Type"), true);
        FLDictKey idKey     = FLDictKey_Init(FLSTR("Persistent ID"), true);
        FLDictKey nameKey   = FLDictKey_Init(FLSTR("Name"), true);
        FLDictKey albumKey  = FLDictKey_Init(FLSTR("Album"), true);
        FLDictKey artistKey = FLDictKey_Init(FLSTR("Artist"), true);
        FLDictKey timeKey   = FLDictKey_Init(FLSTR("Total Time"), true);
        FLDictKey genreKey  = FLDictKey_Init(FLSTR("Genre"), true);
        FLDictKey yearKey   = FLDictKey_Init(FLSTR("Year"), true);
        FLDictKey trackNoKey= FLDictKey_Init(FLSTR("Track Number"), true);
        FLDictKey compKey   = FLDictKey_Init(FLSTR("Compilation"), true);

        TransactionHelper t(db);

        FLEncoder enc = FLEncoder_New();
        FLArrayIterator iter;
        FLArrayIterator_Begin(docs, &iter);
        unsigned numDocs = 0;
        while (FLArrayIterator_Next(&iter)) {
            // Check that track is correct type:
            FLDict track = FLValue_AsDict( FLArrayIterator_GetValue(&iter) );

            FLSlice trackType = FLValue_AsString( FLDict_GetWithKey(track, &typeKey) );
            if (0 != FLSlice_Compare(trackType, FLSTR("File")) &&
                0 != FLSlice_Compare(trackType, FLSTR("Remote")))
                continue;

            FLSlice trackID = FLValue_AsString( FLDict_GetWithKey(track, &idKey) );
            REQUIRE(trackID.buf);

            // Encode doc body:
            FLEncoder_BeginDict(enc, 0);
            REQUIRE(copyValue(track, &nameKey, enc));
            copyValue(track, &albumKey, enc);
            copyValue(track, &artistKey, enc);
            copyValue(track, &timeKey, enc);
            copyValue(track, &genreKey, enc);
            copyValue(track, &yearKey, enc);
            copyValue(track, &trackNoKey, enc);
            copyValue(track, &compKey, enc);
            FLEncoder_EndDict(enc);
            FLError error;
            FLSliceResult body = FLEncoder_Finish(enc, &error);
            REQUIRE(body.buf);
            FLEncoder_Reset(enc);

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
        
        FLEncoder_Free(enc);
        return numDocs;
    }


    void indexViews() {
        FLDictKey nameKey   = FLDictKey_Init(FLSTR("Name"), true);
        FLDictKey albumKey  = FLDictKey_Init(FLSTR("Album"), true);
        FLDictKey artistKey = FLDictKey_Init(FLSTR("Artist"), true);
        FLDictKey timeKey   = FLDictKey_Init(FLSTR("Total Time"), true);
        FLDictKey trackNoKey= FLDictKey_Init(FLSTR("Track Number"), true);
        FLDictKey compKey   = FLDictKey_Init(FLSTR("Compilation"), true);

        auto enc = FLEncoder_New();
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
            FLDict body = FLValue_AsDict( FLValue_FromTrustedData(doc->selectedRev.body) );
            REQUIRE(body);


            FLSlice artist;
            if (FLValue_AsBool(FLDict_GetWithKey(body, &compKey)))
                artist = FLSTR("-Compilations-");
            else
                artist   = FLValue_AsString( FLDict_GetWithKey(body, &artistKey) );
            auto name    = FLValue_AsString( FLDict_GetWithKey(body, &nameKey) );
            auto album   = FLValue_AsString( FLDict_GetWithKey(body, &albumKey) );
            auto trackNo = FLValue_AsInt( FLDict_GetWithKey(body, &trackNoKey) );
            auto time    = FLDict_GetWithKey(body, &timeKey);

            // Generate value:
            FLEncoder_WriteValue(enc, time);
            FLError flError;
            FLSliceResult fval = FLEncoder_Finish(enc, &flError);
            FLEncoder_Reset(enc);
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
        FLEncoder_Free(enc);
        c4key_free(key);
    }


    void indexTracksView() {
        FLDictKey nameKey   = FLDictKey_Init(FLSTR("Name"), true);
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
            FLDict body = FLValue_AsDict( FLValue_FromTrustedData(doc->selectedRev.body) );
            REQUIRE(body);
            auto name    = FLValue_AsString( FLDict_GetWithKey(body, &nameKey) );

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
        FLDictKey likesKey   = FLDictKey_InitWithSharedKeys(FLSTR("likes"),
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
            FLDict body = FLValue_AsDict( FLValue_FromTrustedData(doc->selectedRev.body) );
            REQUIRE(body);
            auto likes = FLValue_AsArray( FLDict_GetWithKey(body, &likesKey) );

            FLArrayIterator iter;
            FLArrayIterator_Begin(likes, &iter);
            unsigned nLikes;
            for (nLikes = 0; nLikes < 3; ++nLikes) {
                FLSlice like = FLValue_AsString(FLArrayIterator_GetValue(&iter));
                if (!like.buf)
                    break;
                c4key_reset(keys[nLikes]);
                c4key_addString(keys[nLikes], like);
                FLArrayIterator_Next(&iter);
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
        FLDictKey contactKey = FLDictKey_InitWithSharedKeys(FLSTR("contact"), sk);
        FLDictKey addressKey = FLDictKey_InitWithSharedKeys(FLSTR("address"), sk);
        FLDictKey stateKey   = FLDictKey_InitWithSharedKeys(FLSTR("state"), sk);
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
            FLDict body = FLValue_AsDict( FLValue_FromTrustedData(doc->selectedRev.body) );
            REQUIRE(body);
            auto contact = FLValue_AsDict( FLDict_GetWithKey(body, &contactKey) );
            auto address = FLValue_AsDict( FLDict_GetWithKey(contact, &addressKey) );
            auto state = FLValue_AsString( FLDict_GetWithKey(address, &stateKey) );

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
        C4Query *query = c4query_new(db, c4str(whereStr), kC4SliceNull, &error);
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
    auto jsonData = readFile(kJSONFilePath);
    FLError error;
    FLSliceResult fleeceData = FLData_ConvertJSON({jsonData.buf, jsonData.size}, &error);
    FLSliceResult_Free(jsonData);
    FLArray root = FLValue_AsArray(FLValue_FromTrustedData((C4Slice)fleeceData));
    unsigned numDocs;

    {
        Stopwatch st;
        numDocs = insertDocs(root);
        CHECK(numDocs == 12188);
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
    auto numDocs = importJSONLines(kLargeDataSetsDir"IPRanges/geoblocks.json",
                                   15.0, true);
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
    sleep(1);//TEMP
}

N_WAY_TEST_CASE_METHOD(PerfTest, "Import names", "[Perf][C][.slow]") {
    // Docs look like:
    // {"name":{"first":"Travis","last":"Mutchler"},"gender":"female","birthday":"1990-12-21","contact":{"address":{"street":"22 Kansas Cir","zip":"45384","city":"Wilberforce","state":"OH"},"email":["Travis.Mutchler@nosql-matters.org","Travis@nosql-matters.org"],"region":"937","phone":["937-3512486"]},"likes":["travelling"],"memberSince":"2010-01-01"}

    __unused auto numDocs = importJSONLines(kLargeDataSetsDir"RandomUsers/names_300000.json", 15.0, true);
    const bool complete = (numDocs == 300000);
#ifdef NDEBUG
    REQUIRE(numDocs == 300000);
#endif
    {
        Stopwatch st;
        auto totalLikes = indexLikesView();
        Log("Total of %u likes\n", totalLikes);
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
                REQUIRE(c4db_createIndex(db, C4STR("contact.address.state"), &error));
                st2.printReport("Creating SQL index of state", 1, "index");
            }
        }
    }
}
