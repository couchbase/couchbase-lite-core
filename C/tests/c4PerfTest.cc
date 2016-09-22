//
//  c4PerfTest.cc
//  LiteCore
//
//  Created by Jens Alfke on 9/20/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#include "c4Test.hh"
#include "Fleece.h"
#include "Base.hh"
#include "Benchmark.hh"
#include <fcntl.h>
#include <assert.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef NDEBUG
#undef REQUIRE  // it slows down the tests significantly
#define REQUIRE(X) ({if (!(X)) abort();})
#endif

using namespace fleece;


static const char* kJSONFilePath = "C/tests/iTunesMusicLibrary.json";

class PerfTest : public C4Test {
public:
    PerfTest(int variation) :C4Test(variation) { }

    ~PerfTest() {
        c4view_free(artistsView);
        c4view_free(albumsView);
    }
    
    alloc_slice readFile(const char *path) {
        int fd = ::open(path, O_RDONLY);
        assert(fd != -1);
        struct stat stat;
        fstat(fd, &stat);
        alloc_slice data(stat.st_size);
        ssize_t bytesRead = ::read(fd, (void*)data.buf, data.size);
        REQUIRE(bytesRead == data.size);
        ::close(fd);
        return data;
    }


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
            rq.docID = {trackID.buf, trackID.size};
            rq.body = {body.buf, body.size};
            rq.save = true;
            C4Document *doc = c4doc_put(db, &rq, nullptr, &c4err);
            REQUIRE(doc != nullptr);
            c4doc_free(doc);
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
            FLDict body = FLValue_AsDict( FLValue_FromTrustedData({doc->selectedRev.body.buf,
                                                                   doc->selectedRev.body.size}) );
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
            C4SliceResult value = {fval.buf, fval.size};

            // Emit to artists view:
            unsigned nKeys = 0;
            if (artist.buf && name.buf) {
                nKeys = 1;
                // Generate key:
                c4key_beginArray(key);
                c4key_addString(key, {artist.buf, artist.size});
                if (album.buf)
                    c4key_addString(key, {album.buf, album.size});
                else
                    c4key_addNull(key);
                c4key_addNumber(key, trackNo);
                c4key_addString(key, {name.buf, name.size});
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
                c4key_addString(key, {album.buf, album.size});
                if (artist.buf)
                    c4key_addString(key, {artist.buf, artist.size});
                else
                    c4key_addNull(key);
                c4key_addNumber(key, trackNo);
                if (!name.buf)
                    name = FLSTR("");
                c4key_addString(key, {name.buf, name.size});
                c4key_addNumber(key, 1);
                c4key_endArray(key);
            }
            REQUIRE(c4indexer_emit(indexer, doc, 1, nKeys, &key, &value, &error));
            c4key_reset(key);

            c4slice_free(value);
            c4doc_free(doc);
        }
        c4enum_free(e);
        REQUIRE(error.code == 0);

        REQUIRE(c4indexer_end(indexer, true, &error));
        FLEncoder_Free(enc);
        c4key_free(key);
    }


    unsigned queryTracks() {
        std::vector<std::string> allArtists;
        allArtists.reserve(1200);

        C4QueryOptions options = kC4DefaultQueryOptions;
        //options.groupLevel = 1;   //TODO: Implement grouping :)
        C4Error error;
        auto query = c4view_query(artistsView, &options, &error);
        while (c4queryenum_next(query, &error)) {
            C4KeyReader key = query->key;
            REQUIRE(c4key_peek(&key) == kC4Array);
            c4key_skipToken(&key);
            C4Slice artistSlice = c4key_readString(&key);
            REQUIRE(artistSlice.buf);
            allArtists.push_back(std::string((const char*)artistSlice.buf, artistSlice.size));
        }
        c4queryenum_free(query);
        return (unsigned) allArtists.size();
    }


    C4View *artistsView {nullptr};
    C4View *albumsView {nullptr};
};


N_WAY_TEST_CASE_METHOD(PerfTest, "Performance", "[Perf][C]") {
    auto jsonData = readFile(kJSONFilePath);
    FLError error;
    auto fleeceData = FLData_ConvertJSON({jsonData.buf, jsonData.size}, &error);
    FLArray root = FLValue_AsArray(FLValue_FromTrustedData({fleeceData.buf, fleeceData.size}));
    unsigned numDocs;

    {
        Stopwatch st;
        numDocs = insertDocs(root);
        double ms = st.elapsedMS();
        fprintf(stderr, "Writing %u docs took %.3f ms (%.3f us/doc, or %.0f docs/sec)\n",
                numDocs, ms, ms/numDocs*1000.0, numDocs/ms*1000);
    }
    FLSlice_Free(fleeceData);
    {
        Stopwatch st;
        indexViews();
        double ms = st.elapsedMS();
        fprintf(stderr, "Indexing %u docs took %.3f ms (%.3f us/doc, or %.0f docs/sec)\n",
                numDocs, ms, ms/numDocs*1000.0, numDocs/ms*1000);
    }
    {
        Stopwatch st;
        queryTracks();
        double ms = st.elapsedMS();
        fprintf(stderr, "Querying %u tracks took %.3f ms (%.3f us/doc, or %.0f docs/sec)\n",
                numDocs, ms, ms/numDocs*1000.0, numDocs/ms*1000);
    }
}
