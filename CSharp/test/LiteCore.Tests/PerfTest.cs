using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;

using FluentAssertions;
using LiteCore.Interop;
using LiteCore.Tests.Util;
using LiteCore.Util;
using Xunit;

namespace LiteCore.Tests
{
    public unsafe class PerfTest : Test
    {
        private class TotalContext
        {
            public double total;
        }

        private const string JsonFilePath = "../../../C/tests/iTunesMusicLibrary.json";
        private C4View *_artistsView;
        private C4View *_albumsView;
        private C4View *_tracksView;
        private C4View *_likesView;
        private C4View *_statesView;

        [Fact]
        public void TestPerformance()
        {
            var jsonData = File.ReadAllBytes(JsonFilePath);
            FLError error;
            var fleeceData = Native.FLData_ConvertJSON(jsonData, &error);
            var root = Native.FLValue_AsArray(Native.FLValue_FromData(fleeceData));

            RunTestVariants(() => {
                uint numDocs;
                {
                    var st = Stopwatch.StartNew();
                    numDocs = InsertDocs(root);
                    numDocs.Should().Be(12188, "because otherwise an incorrect number of documents was inserted");
                    st.PrintReport("Writing docs", numDocs, "doc");
                }
                {
                    var st = Stopwatch.StartNew();
                    IndexViews();
                    st.PrintReport("Indexing Albums/Artists views", numDocs, "doc");
                }
                {
                    var st = Stopwatch.StartNew();
                    IndexTracksView();
                    st.PrintReport("Indexing Tracks view", numDocs, "doc");
                }
                {
                    var st = Stopwatch.StartNew();
                    var context = new TotalContext();
                    var reduce = new C4ManagedReduceFunction(TotalAccumulate, TotalReduce, context);
                    
                    var numArtists = QueryGrouped(_artistsView, reduce.Native);
                    reduce.Dispose();
                    numArtists.Should().Be(1141, "because otherwise the query returned incorrect information");
                    st.PrintReport("Grouped query of Artist view", numDocs, "doc");
                }
            });
        }

        private uint QueryGrouped(C4View* view, C4ReduceFunction reduce, bool verbose = false)
        {
            var options = C4QueryOptions.Default;
            options.reduce = &reduce;
            options.groupLevel = 1;
            return RunQuery(view, options, verbose);
        }

        private uint RunQuery(C4View* view, C4QueryOptions options, bool verbose = false)
        {
            var allKeys = new List<string>(1200);
            C4Error error;
            var query = (C4QueryEnumerator *)LiteCoreBridge.Check(err => {
                var localOpts = options;
                return Native.c4view_query(view, &localOpts, err);
            });

            C4SliceResult keySlice;
            while(Native.c4queryenum_next(query, &error)) {
                var key = query->key;
                if(Native.c4key_peek(&key) == C4KeyToken.Array) {
                    Native.c4key_skipToken(&key);
                    keySlice = NativeRaw.c4key_readString(&key);
                } else {
                    Native.c4key_peek(&key).Should().Be(C4KeyToken.String, "because otherwise an invalid entry is present");
                    keySlice = NativeRaw.c4key_readString(&key);
                }

                ((long)keySlice.buf).Should().NotBe(0, "because the entry should not be null");
                var keyStr = ((C4Slice)keySlice).CreateString();
                if(verbose) {
                    var valStr = query->value.CreateString();
                    Console.Write($"{keyStr} ({valStr}) ");
                }

                allKeys.Add(keyStr);
                Native.c4slice_free(keySlice);
            }

            Native.c4queryenum_free(query);
            if(verbose) {
                Console.WriteLine();
            }

            return (uint)allKeys.Count;
        }

        private uint InsertDocs(FLArray* docs)
        {
            var typeKey   = NativeRaw.FLDictKey_Init(FLSlice.Constant("Track Type"), true);
            var idKey     = NativeRaw.FLDictKey_Init(FLSlice.Constant("Persistent ID"), true);
            var nameKey   = NativeRaw.FLDictKey_Init(FLSlice.Constant("Name"), true);
            var albumKey  = NativeRaw.FLDictKey_Init(FLSlice.Constant("Album"), true);
            var artistKey = NativeRaw.FLDictKey_Init(FLSlice.Constant("Artist"), true);
            var timeKey   = NativeRaw.FLDictKey_Init(FLSlice.Constant("Total Time"), true);
            var genreKey  = NativeRaw.FLDictKey_Init(FLSlice.Constant("Genre"), true);
            var yearKey   = NativeRaw.FLDictKey_Init(FLSlice.Constant("Year"), true);
            var trackNoKey= NativeRaw.FLDictKey_Init(FLSlice.Constant("Track Number"), true);
            var compKey   = NativeRaw.FLDictKey_Init(FLSlice.Constant("Compilation"), true);

            LiteCoreBridge.Check(err => Native.c4db_beginTransaction(_db, err));
            try {
                var enc = Native.FLEncoder_New();
                FLArrayIterator iter;
                Native.FLArrayIterator_Begin(docs, &iter);
                uint numDocs = 0;
                while(Native.FLArrayIterator_Next(&iter)) {
                    // Check that track is correct type:
                    var track = Native.FLValue_AsDict(Native.FLArrayIterator_GetValue(&iter));
                    var trackType = Native.FLValue_AsString(Native.FLDict_GetWithKey(track, &typeKey));
                    if(trackType != "File" && trackType != "Remote") {
                        continue;
                    }

                    var trackID = Native.FLValue_AsString(Native.FLDict_GetWithKey(track, &idKey));
                    trackID.Should().NotBeNull("because otherwise the data was not read correctly");

                    // Encode doc body:
                    Native.FLEncoder_BeginDict(enc, 0);
                    CopyValue(track, &nameKey, enc).Should().BeTrue("because otherwise the copy failed");
                    CopyValue(track, &albumKey, enc);
                    CopyValue(track, &artistKey, enc);
                    CopyValue(track, &timeKey, enc);
                    CopyValue(track, &genreKey, enc);
                    CopyValue(track, &yearKey, enc);
                    CopyValue(track, &trackNoKey, enc);
                    CopyValue(track, &compKey, enc);
                    Native.FLEncoder_EndDict(enc);
                    FLError err;
                    var body = Native.FLEncoder_Finish(enc, &err);
                    body.Should().NotBeNull("because otherwise the encoding process failed");
                    Native.FLEncoder_Reset(enc);

                    // Save Document:
                    using(var trackID_ = new C4String(trackID)) {
                        fixed(byte* b = body) {
                            var rq = new C4DocPutRequest();
                            rq.docID = trackID_.AsC4Slice();
                            rq.body = new C4Slice(b, (ulong)body.Length);
                            rq.save = true;
                            var doc = (C4Document *)LiteCoreBridge.Check(c4err => {
                                var localRq = rq;
                                return Native.c4doc_put(_db, &localRq, null, c4err);
                            });
                            
                            Native.c4doc_free(doc);
                            ++numDocs;
                        }
                    }
                }

                Native.FLEncoder_Free(enc);
                return numDocs;
            } finally {
                LiteCoreBridge.Check(err => Native.c4db_endTransaction(_db, true, err));
            }
        }

        private void IndexViews()
        {
            var nameKey   = NativeRaw.FLDictKey_Init(FLSlice.Constant("Name"), true);
            var albumKey  = NativeRaw.FLDictKey_Init(FLSlice.Constant("Album"), true);
            var artistKey = NativeRaw.FLDictKey_Init(FLSlice.Constant("Artist"), true);
            var timeKey   = NativeRaw.FLDictKey_Init(FLSlice.Constant("Total Time"), true);
            var trackNoKey= NativeRaw.FLDictKey_Init(FLSlice.Constant("Track Number"), true);
            var compKey   = NativeRaw.FLDictKey_Init(FLSlice.Constant("Compilation"), true);

            var enc = Native.FLEncoder_New();
            var key = Native.c4key_new();

            C4Error error;
            if(_artistsView == null) {
                var config = Native.c4db_getConfig(_db);
                _artistsView = (C4View *)LiteCoreBridge.Check(err => Native.c4view_open(_db, null, "Artists", "1", 
                    Native.c4db_getConfig(_db), err));
            }

            if(_albumsView == null) {
                _albumsView = (C4View *)LiteCoreBridge.Check(err => Native.c4view_open(_db, null, "Albums", "1", 
                    Native.c4db_getConfig(_db), err));
            }

            var views = new[] { _artistsView, _albumsView };
            var indexer = (C4Indexer *)LiteCoreBridge.Check(err => Native.c4indexer_begin(_db, views, err));
            var e = (C4DocEnumerator *)LiteCoreBridge.Check(err => Native.c4indexer_enumerateDocuments(indexer, err));
            while(Native.c4enum_next(e, &error)) {
                var doc = Native.c4enum_getDocument(e, &error);
                var body = Native.FLValue_AsDict(NativeRaw.FLValue_FromTrustedData((FLSlice)doc->selectedRev.body));
                ((long)body).Should().NotBe(0, "because otherwise the data got corrupted somehow");
                
                string artist;
                if(Native.FLValue_AsBool(Native.FLDict_GetWithKey(body, &compKey))) {
                    artist = "-Compilations-";
                } else {
                    artist = Native.FLValue_AsString(Native.FLDict_GetWithKey(body, &artistKey));
                }

                var name = Native.FLValue_AsString(Native.FLDict_GetWithKey(body, &nameKey));
                var album = Native.FLValue_AsString(Native.FLDict_GetWithKey(body, &albumKey));
                var trackNo = Native.FLValue_AsInt(Native.FLDict_GetWithKey(body, &trackNoKey));
                var time = Native.FLDict_GetWithKey(body, &timeKey);

                // Generate Value:
                Native.FLEncoder_WriteValue(enc, time);
                FLError flError;
                var fval = NativeRaw.FLEncoder_Finish(enc, &flError);
                Native.FLEncoder_Reset(enc);
                ((long)fval.buf).Should().NotBe(0, "because otherwise the encoding failed");
                var value = (C4Slice)fval;

                // Emit to artists view:
                if(artist != null && name != null) {
                    // Generate key:
                    Native.c4key_beginArray(key);
                    Native.c4key_addString(key, artist);
                    if(album != null) {
                        Native.c4key_addString(key, album);
                    } else {
                        Native.c4key_addNull(key);
                    }

                    Native.c4key_addNumber(key, trackNo);
                    Native.c4key_addString(key, name);
                    Native.c4key_addNumber(key, 1.0);
                    Native.c4key_endArray(key);
                }

                Native.c4indexer_emit(indexer, doc, 0, new[] { key }, new[] { value }, &error).Should()
                    .BeTrue("because otherwise the emit to the artists view failed");
                Native.c4key_reset(key);

                // Emit to albums view:
                if(album != null) {
                    Native.c4key_beginArray(key);
                    Native.c4key_addString(key, album);
                    if(artist != null) {
                        Native.c4key_addString(key, artist);
                    } else {
                        Native.c4key_addNull(key);
                    }

                    Native.c4key_addString(key, name);
                    Native.c4key_addNumber(key, 1.0);
                    Native.c4key_endArray(key);
                }

                Native.c4indexer_emit(indexer, doc, 1, new[] { key }, new[] { value }, &error).Should()
                    .BeTrue("because otherwise the emit to the artists view failed");
                Native.c4key_reset(key);

                Native.FLSliceResult_Free(fval);
                Native.c4doc_free(doc);
            }

            Native.c4enum_free(e);
            error.Code.Should().Be(0, "because otherwise an error occurred");
            Native.c4indexer_end(indexer, true, &error).Should().BeTrue("because otherwise the indexer failed to end");
            Native.FLEncoder_Free(enc);
            Native.c4key_free(key);
        }

        private void IndexTracksView()
        {
            var nameKey = NativeRaw.FLDictKey_Init(FLSlice.Constant("Name"), true);
            var key = Native.c4key_new();

            C4Error error;
            if(_tracksView == null) {
                _tracksView = (C4View *)LiteCoreBridge.Check(err => Native.c4view_open(_db, null, "Tracks", "1",
                    Native.c4db_getConfig(_db), err));
            }

            var views = new[] { _tracksView };
            var indexer = (C4Indexer *)LiteCoreBridge.Check(err => Native.c4indexer_begin(_db, views, err));
            try {
                var e = (C4DocEnumerator *)LiteCoreBridge.Check(err => Native.c4indexer_enumerateDocuments(indexer, err));
                while(Native.c4enum_next(e, &error)) {
                    var doc = Native.c4enum_getDocument(e, &error);
                    var body = Native.FLValue_AsDict(NativeRaw.FLValue_FromTrustedData((FLSlice)doc->selectedRev.body));
                    ((long)body).Should().NotBe(0, "because otherwise the data got corrupted somehow");
                    var name = Native.FLValue_AsString(Native.FLDict_GetWithKey(body, &nameKey));

                    Native.c4key_reset(key);
                    Native.c4key_addString(key, name);

                    var value = C4Slice.Null;
                    LiteCoreBridge.Check(err => Native.c4indexer_emit(indexer, doc, 0, new[] { key }, new[] { value }, err));
                    Native.c4key_reset(key);
                    Native.c4doc_free(doc);
                }

                Native.c4enum_free(e);
                error.Code.Should().Be(0, "because otherwise an error occurred somewhere");
            } finally {
                LiteCoreBridge.Check(err => Native.c4indexer_end(indexer, true, err));
                Native.c4key_free(key);
            }
        }

        private static void TotalAccumulate(object context, C4Key* key, C4Slice value)
        {
            var ctx = context as TotalContext;
            var v = NativeRaw.FLValue_FromTrustedData((FLSlice)value);
            Native.FLValue_GetType(v).Should().Be(FLValueType.Number, "because otherwise invalid data was indexed");
            ctx.total += Native.FLValue_AsDouble(v);
        }

        private static string TotalReduce(object context)
        {
            var ctx = context as TotalContext;
            return ctx.total.ToString();
        }

        private static bool CopyValue(FLDict* source, FLDictKey* key, FLEncoder* enc)
        {
            var value = Native.FLDict_GetWithKey(source, key);
            if(value == null) {
                return false;
            }

            Native.FLEncoder_WriteKey(enc, Native.FLDictKey_GetString(key));
            Native.FLEncoder_WriteValue(enc, value);
            return true;
        }

        protected override void TeardownVariant(int option)
        {
            base.TeardownVariant(option);

            if(_artistsView != null) {
                LiteCoreBridge.Check(err => Native.c4view_close(_artistsView, err));
                LiteCoreBridge.Check(err => Native.c4view_delete(_artistsView, err));
                Native.c4view_free(_artistsView);
                _artistsView = null;
            }

            if(_albumsView != null) {
                LiteCoreBridge.Check(err => Native.c4view_close(_albumsView, err));
                LiteCoreBridge.Check(err => Native.c4view_delete(_albumsView, err));
                Native.c4view_free(_albumsView);
                _albumsView = null;
            }

            if(_tracksView != null) {
                LiteCoreBridge.Check(err => Native.c4view_close(_tracksView, err));
                LiteCoreBridge.Check(err => Native.c4view_delete(_tracksView, err));
                Native.c4view_free(_tracksView);
                _tracksView = null;
            }

            if(_likesView != null) {
                LiteCoreBridge.Check(err => Native.c4view_close(_likesView, err));
                LiteCoreBridge.Check(err => Native.c4view_delete(_likesView, err));
                Native.c4view_free(_likesView);
                _likesView = null;
            }

            if(_statesView != null) {
                LiteCoreBridge.Check(err => Native.c4view_close(_statesView, err));
                LiteCoreBridge.Check(err => Native.c4view_delete(_statesView, err));
                Native.c4view_free(_statesView);
                _statesView = null;
            }
        }
    }
}