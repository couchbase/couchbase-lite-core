using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
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

        private class CountContext
        {
            public ulong count;
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
            var unmanagedData = Marshal.AllocHGlobal(jsonData.Length);
            Marshal.Copy(jsonData, 0, unmanagedData, jsonData.Length);
            var slice = new FLSlice(unmanagedData.ToPointer(), (ulong)jsonData.Length);
            var fleeceData = NativeRaw.FLData_ConvertJSON(slice, &error);
            var root = Native.FLValue_AsArray(NativeRaw.FLValue_FromData(fleeceData));

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

            Marshal.FreeHGlobal(unmanagedData);
        }

        [Fact]
        public void TestImportGeoBlocks()
        {
            var rng = new Random();
            RunTestVariants(() => {
                var numDocs = ImportJSONLines("/Couchbase/example-datasets-master/IPRanges/geoblocks.json",
                    TimeSpan.FromSeconds(15), true);
                ReopenDB();
                var st = Stopwatch.StartNew();
                int readNo = 0;
                for( ; readNo < 100000; ++readNo) {
                    var docID = rng.Next(1, (int)numDocs + 1).ToString("D7");
                    var doc = (C4Document *)LiteCoreBridge.Check(err => Native.c4doc_get(Db, docID,
                        true, err));
                    doc->selectedRev.body.size.Should().BeGreaterThan(10);
                    Native.c4doc_free(doc);
                }

                st.PrintReport("Reading random docs", (uint)readNo, "doc");
            });
        }

        [Fact]
        public void TestImportNames()
        {
            // Docs look like:
            // {"name":{"first":"Travis","last":"Mutchler"},"gender":"female","birthday":"1990-12-21","contact":{"address":{"street":"22 Kansas Cir","zip":"45384","city":"Wilberforce","state":"OH"},"email":["Travis.Mutchler@nosql-matters.org","Travis@nosql-matters.org"],"region":"937","phone":["937-3512486"]},"likes":["travelling"],"memberSince":"2010-01-01"}

            RunTestVariants(() => {
                var numDocs = ImportJSONLines("/Couchbase/example-datasets-master/RandomUsers/names_300000.json",
                    TimeSpan.FromSeconds(15), true);
                var complete = numDocs == 300000;
                #if !DEBUG
                numDocs.Should().Be(300000, "because otherwise the operation was too slow");
                #endif
                {
                    var st = Stopwatch.StartNew();
                    var totalLikes = IndexLikesView();
                    Console.WriteLine($"Total of {totalLikes} likes");
                    st.PrintReport("Indexing Likes view", numDocs, "doc");
                    if(complete) {
                        totalLikes.Should().Be(345986, "because otherwise the index missed data set objects");
                    }
                }
                {
                    var st = Stopwatch.StartNew();
                    var context = new CountContext();
                    using(var reduce = new C4ManagedReduceFunction(CountAccumulate, CountReduce, context)) {
                        var numLikes = QueryGrouped(_likesView, reduce.Native, true);
                        st.PrintReport("Querying all likes", numLikes, "like");
                        if(complete) {
                            numLikes.Should().Be(15, "because that is the number of likes in the data set");
                        }
                    }
                }
                {
                    var st = Stopwatch.StartNew();
                    var total = IndexStatesView();
                    st.PrintReport("Indexing States view", numDocs, "doc");
                    if(complete) {
                        total.Should().Be(300000, "because otherwise the index missed some dataset objects");
                    }
                }
                {
                    var options = C4QueryOptions.Default;
                    var key = Native.c4key_new();
                    NativeRaw.c4key_addString(key, C4Slice.Constant("WA"));
                    options.startKey = options.endKey = key;
                    var st = Stopwatch.StartNew();
                    var total = RunQuery(_statesView, options);
                    Native.c4key_free(key);
                    st.PrintReport("Querying States view", total, "row");
                    if(complete) {
                        total.Should().Be(5053, "because that is the number of states in the data set");
                    }
                }
                {
                    if(Storage == C4StorageEngine.SQLite && !IsRevTrees()) {
                        for(int pass = 0; pass < 2; ++pass) {
                            var st = Stopwatch.StartNew();
                            var n = QueryWhere("{\"contact.address.state\": \"WA\"}");
                            st.PrintReport("SQL query of state", n, "doc");
                            if(complete) {
                                n.Should().Be(5053, "because that is the number of states in the data set"); 
                            }
                            if(pass == 0) {
                                var st2 = Stopwatch.StartNew();
                                LiteCoreBridge.Check(err => Native.c4db_createIndex(Db, "contact.address.state", C4IndexType.Value, null, err));
                                st2.PrintReport("Creating SQL index of state", 1, "index");
                            }
                        }
                    }
                }
            });
        }

        private uint QueryWhere(string whereStr, bool verbose = false)
        {
            var docIDs = new List<string>(1200);

            var query = (C4Query *)LiteCoreBridge.Check(err => Native.c4query_new(Db, whereStr, null, err));
            var e = (C4QueryEnumerator *)LiteCoreBridge.Check(err => Native.c4query_run(query, null, null, err));
            string artist;
            C4Error error;
            while(Native.c4queryenum_next(e, &error)) {
                artist = e->docID.CreateString();
                if(verbose) {
                    Console.Write($"{artist}  ");
                }

                docIDs.Add(artist);
            }

            Native.c4queryenum_free(e);
            Native.c4query_free(query);
            if(verbose) {
                Console.WriteLine();
            }

            return (uint)docIDs.Count;
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

            LiteCoreBridge.Check(err => Native.c4db_beginTransaction(Db, err));
            try {
                var enc = Native.FLEncoder_New();
                FLArrayIterator iter;
                Native.FLArrayIterator_Begin(docs, &iter);
                uint numDocs = 0;
                while(Native.FLArrayIterator_Next(&iter)) {
                    // Check that track is correct type:
                    var track = Native.FLValue_AsDict(Native.FLArrayIterator_GetValue(&iter));
                    var trackType = NativeRaw.FLValue_AsString(Native.FLDict_GetWithKey(track, &typeKey));
                    if(!trackType.Equals(FLSlice.Constant("File")) && !trackType.Equals(FLSlice.Constant("Remote"))) {
                        continue;
                    }

                    var trackID = NativeRaw.FLValue_AsString(Native.FLDict_GetWithKey(track, &idKey));
                    ((long)trackID.buf).Should().NotBe(0, "because otherwise the data was not read correctly");

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
                    var body = NativeRaw.FLEncoder_Finish(enc, &err);
                    body.Should().NotBeNull("because otherwise the encoding process failed");
                    Native.FLEncoder_Reset(enc);

                    // Save Document:
                    var rq = new C4DocPutRequest();
                    rq.docID = (C4Slice)trackID;
                    rq.body = body;
                    rq.save = true;
                    var doc = (C4Document *)LiteCoreBridge.Check(c4err => {
                        var localRq = rq;
                        return Native.c4doc_put(Db, &localRq, null, c4err);
                    });
                    
                    Native.c4doc_free(doc);
                    ++numDocs;
                }

                Native.FLEncoder_Free(enc);
                return numDocs;
            } finally {
                LiteCoreBridge.Check(err => Native.c4db_endTransaction(Db, true, err));
            }
        }

        private void IndexViews()
        {
            var nameKey   = Native.FLDictKey_Init("Name", true);
            var albumKey  = Native.FLDictKey_Init("Album", true);
            var artistKey = Native.FLDictKey_Init("Artist", true);
            var timeKey   = Native.FLDictKey_Init("Total Time", true);
            var trackNoKey= Native.FLDictKey_Init("Track Number", true);
            var compKey   = Native.FLDictKey_Init("Compilation", true);

            var enc = Native.FLEncoder_New();
            var key = Native.c4key_new();

            C4Error error;
            if(_artistsView == null) {
                var config = Native.c4db_getConfig(Db);
                _artistsView = (C4View *)LiteCoreBridge.Check(err => Native.c4view_open(Db, null, "Artists", "1", 
                    Native.c4db_getConfig(Db), err));
            }

            if(_albumsView == null) {
                _albumsView = (C4View *)LiteCoreBridge.Check(err => Native.c4view_open(Db, null, "Albums", "1", 
                    Native.c4db_getConfig(Db), err));
            }

            var views = new[] { _artistsView, _albumsView };
            var indexer = (C4Indexer *)LiteCoreBridge.Check(err => Native.c4indexer_begin(Db, views, err));
            var e = (C4DocEnumerator *)LiteCoreBridge.Check(err => Native.c4indexer_enumerateDocuments(indexer, err));
            while(Native.c4enum_next(e, &error)) {
                var doc = Native.c4enum_getDocument(e, &error);
                var body = Native.FLValue_AsDict(NativeRaw.FLValue_FromTrustedData((FLSlice)doc->selectedRev.body));
                ((long)body).Should().NotBe(0, "because otherwise the data got corrupted somehow");
                
                FLSlice artist;
                if(Native.FLValue_AsBool(Native.FLDict_GetWithKey(body, &compKey))) {
                    artist = FLSlice.Constant("-Compilations-");
                } else {
                    artist = NativeRaw.FLValue_AsString(Native.FLDict_GetWithKey(body, &artistKey));
                }

                var name = NativeRaw.FLValue_AsString(Native.FLDict_GetWithKey(body, &nameKey));
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
                uint nKeys = 0;
                if(!artist.Equals(FLSlice.Null) && !name.Equals(FLSlice.Null)) {
                    nKeys = 1;
                    // Generate key:
                    Native.c4key_beginArray(key);
                    NativeRaw.c4key_addString(key, (C4Slice)artist);
                    if(album != null) {
                        Native.c4key_addString(key, album);
                    } else {
                        Native.c4key_addNull(key);
                    }

                    Native.c4key_addNumber(key, trackNo);
                    NativeRaw.c4key_addString(key, (C4Slice)name);
                    Native.c4key_addNumber(key, 1.0);
                    Native.c4key_endArray(key);
                }

                NativeRaw.c4indexer_emit(indexer, doc, 0, nKeys, &key, new[] { value }, &error).Should()
                    .BeTrue("because otherwise the emit to the artists view failed");
                Native.c4key_reset(key);

                // Emit to albums view:
                nKeys = 0;
                if(album != null) {
                    nKeys = 1;
                    Native.c4key_beginArray(key);
                    Native.c4key_addString(key, album);
                    if(!artist.Equals(FLSlice.Null)) {
                        NativeRaw.c4key_addString(key, (C4Slice)artist);
                    } else {
                        Native.c4key_addNull(key);
                    }

                    Native.c4key_addNumber(key, trackNo);
                    if(name.buf == null) {
                        name = FLSlice.Constant("");
                    }

                    NativeRaw.c4key_addString(key, (C4Slice)name);
                    Native.c4key_addNumber(key, 1.0);
                    Native.c4key_endArray(key);
                }

                NativeRaw.c4indexer_emit(indexer, doc, 1, nKeys, &key, new[] { value }, &error).Should()
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
                _tracksView = (C4View *)LiteCoreBridge.Check(err => Native.c4view_open(Db, null, "Tracks", "1",
                    Native.c4db_getConfig(Db), err));
            }

            var views = new[] { _tracksView };
            var indexer = (C4Indexer *)LiteCoreBridge.Check(err => Native.c4indexer_begin(Db, views, err));
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
                    LiteCoreBridge.Check(err => NativeRaw.c4indexer_emit(indexer, doc, 0, new[] { key }, new[] { value }, err));
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

        private uint IndexLikesView()
        {
            var likesKey = Native.FLDictKey_Init("likes", true);
            var keys = new[] { Native.c4key_new(), Native.c4key_new(), Native.c4key_new() };
            var values = new C4Slice[3];
            uint totalLikes = 0;

            if(_likesView == null) {
                _likesView = (C4View *)LiteCoreBridge.Check(err => Native.c4view_open(Db, null, "likes",
                    "1", Native.c4db_getConfig(Db), err));
            }

            var views = new[] { _likesView };
            var indexer = (C4Indexer *)LiteCoreBridge.Check(err => Native.c4indexer_begin(Db, views, err));
            var e = (C4DocEnumerator *)LiteCoreBridge.Check(err => Native.c4indexer_enumerateDocuments(indexer, err));
            C4Error error;
            while(Native.c4enum_next(e, &error)) {
                var doc = (C4Document *)LiteCoreBridge.Check(err => Native.c4enum_getDocument(e, err));
                var body = Native.FLValue_AsDict(NativeRaw.FLValue_FromTrustedData((FLSlice)doc->selectedRev.body));
                var likes = Native.FLValue_AsArray(Native.FLDict_GetWithKey(body, &likesKey));

                FLArrayIterator iter;
                Native.FLArrayIterator_Begin(likes, &iter);
                uint nLikes;
                for(nLikes = 0; nLikes < 3; ++nLikes) {
                    var like = NativeRaw.FLValue_AsString(Native.FLArrayIterator_GetValue(&iter));
                    if(like.buf == null) {
                        break;
                    }

                    Native.c4key_reset(keys[nLikes]);
                    NativeRaw.c4key_addString(keys[nLikes], (C4Slice)like);
                    Native.FLArrayIterator_Next(&iter);
                }

                totalLikes += nLikes;
                LiteCoreBridge.Check(err => {
                    var localKeys = keys;
                    fixed(C4Key** keys_ = localKeys) {
                        return NativeRaw.c4indexer_emit(indexer, doc, 0, nLikes, keys_, values, err);
                    }
                });
        
                Native.c4doc_free(doc);
            }

            Native.c4enum_free(e);
            error.Code.Should().Be(0, "because otherwise an error occurred somewhere");
            LiteCoreBridge.Check(err => Native.c4indexer_end(indexer, true, err));
            for(uint i = 0; i < 3; i++) {
                Native.c4key_free(keys[i]);
            }

            return totalLikes;
        }

        private uint IndexStatesView()
        {
            var contactKey = Native.FLDictKey_Init("contact", true);
            var addressKey = Native.FLDictKey_Init("address", true);
            var stateKey = Native.FLDictKey_Init("state", true);
            var key = Native.c4key_new();
            uint totalStates = 0;

            if(_statesView == null) {
                _statesView = (C4View *)LiteCoreBridge.Check(err => Native.c4view_open(Db, null, "states",
                    "1", Native.c4db_getConfig(Db), err));
            }

            var views = new[] { _statesView };
            var indexer = (C4Indexer *)LiteCoreBridge.Check(err => Native.c4indexer_begin(Db, views, err));
            var e = (C4DocEnumerator *)LiteCoreBridge.Check(err => Native.c4indexer_enumerateDocuments(indexer, err));
            C4Error error;
            while(Native.c4enum_next(e, &error)) {
                var doc = (C4Document *)LiteCoreBridge.Check(err => Native.c4enum_getDocument(e, err));
                var body = Native.FLValue_AsDict(NativeRaw.FLValue_FromTrustedData((FLSlice)doc->selectedRev.body));
                var contact = Native.FLValue_AsDict(Native.FLDict_GetWithKey(body, &contactKey));
                var address = Native.FLValue_AsDict(Native.FLDict_GetWithKey(contact, &addressKey));
                var state = NativeRaw.FLValue_AsString(Native.FLDict_GetWithKey(address, &stateKey));

                uint nStates = 0;
                if(state.buf != null) {
                    Native.c4key_reset(key);
                    NativeRaw.c4key_addString(key, (C4Slice)state);
                    nStates = 1;
                    totalStates++;
                }

                var value = C4Slice.Null;
                LiteCoreBridge.Check(err => {
                    var localKey = key;
                    return NativeRaw.c4indexer_emit(indexer, doc, 0, nStates, &localKey, 
                        new[] { value }, err);
                });
                Native.c4doc_free(doc);
            }

            Native.c4enum_free(e);
            error.Code.Should().Be(0, "because otherwise an error occurred somewhere");
            LiteCoreBridge.Check(err => Native.c4indexer_end(indexer, true, err));
            Native.c4key_free(key);
            return totalStates;
        }

        private static void TotalAccumulate(object context, C4Key* key, C4Slice value)
        {
            var ctx = context as TotalContext;
            var v = NativeRaw.FLValue_FromTrustedData((FLSlice)value);
            ctx.total += Native.FLValue_AsDouble(v);
        }

        private static string TotalReduce(object context)
        {
            var ctx = context as TotalContext;
            var retVal = ctx.total.ToString("G6");
            ctx.total = 0.0;
            return retVal;
        }

        private static void CountAccumulate(object context, C4Key* key, C4Slice value)
        {
            (context as CountContext).count++;
        }

        private static string CountReduce(object context)
        {
            var ctx = context as CountContext;
            var retVal = ctx.ToString();
            ctx.count = 0;
            return retVal;
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
            Native.c4view_free(_artistsView);
            _artistsView = null;
            Native.c4view_free(_albumsView);
            _albumsView = null;
            Native.c4view_free(_tracksView);
            _tracksView = null;
            Native.c4view_free(_likesView);
            _likesView = null;
            Native.c4view_free(_statesView);
            _statesView = null;

            base.TeardownVariant(option);
        }
    }
}