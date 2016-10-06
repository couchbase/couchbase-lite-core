using System;
using System.Runtime.InteropServices;
using FluentAssertions;
using LiteCore.Interop;
using Xunit;

namespace LiteCore.Tests
{
    public unsafe class ViewTest : Test
    {
        private class CountContext
        {
            public uint count;
        }

        private C4View* _view;

        [Fact]
        public void TestEmptyState()
        {
            RunTestVariants(() => {
                Native.c4view_getTotalRows(_view).Should().Be(0, "because an empty view has no rows");
                Native.c4view_getLastSequenceIndexed(_view).Should().Be(0, "because an empty view has no index entries");
                Native.c4view_getLastSequenceChangedAt(_view).Should().Be(0, "because an empty view has no changes");
            });
        }

        [Fact]
        public void TestCreateIndex()
        {
            RunTestVariants(() => {
                CreateIndex();

                Native.c4view_getTotalRows(_view).Should().Be(200, "because the view should have two entries per document");
                Native.c4view_getLastSequenceIndexed(_view).Should().Be(100, "because the last sequence index should be the highest existing");
                Native.c4view_getLastSequenceChangedAt(_view).Should().Be(100, "because the last sequence changed should be the highest existing");
            });
        }

        [Fact]
        public void TestIndexVersion()
        {
            RunTestVariants(() => {
                CreateIndex();

                // Reopen view with same version string:
                LiteCoreBridge.Check(err => Native.c4view_close(_view, err));
                Native.c4view_free(_view);

                _view = (C4View *)LiteCoreBridge.Check(err => Native.c4view_open(_db, null, "myview", "1",
                    Native.c4db_getConfig(_db), err));
                Native.c4view_getTotalRows(_view).Should().Be(200, "because the view information should survive reopening");
                Native.c4view_getLastSequenceIndexed(_view).Should().Be(100, "because the view information should survive reopening");
                Native.c4view_getLastSequenceChangedAt(_view).Should().Be(100, "because the view information should survive reopening");
                
                // Reopen view with different version string:
                LiteCoreBridge.Check(err => Native.c4view_close(_view, err));
                Native.c4view_free(_view);

                _view = (C4View *)LiteCoreBridge.Check(err => Native.c4view_open(_db, null, "myview", "2",
                    Native.c4db_getConfig(_db), err));
                Native.c4view_getTotalRows(_view).Should().Be(0, "because the view information should be erased");
                Native.c4view_getLastSequenceIndexed(_view).Should().Be(0, "because the view information should be erased");
                Native.c4view_getLastSequenceChangedAt(_view).Should().Be(0, "because the view information should be erased");
            });
        }

        [Fact]
        public void TestQueryIndex()
        {
            RunTestVariants(() => {
                CreateIndex();

                var e = (C4QueryEnumerator *)LiteCoreBridge.Check(err => Native.c4view_query(_view, null, err));

                int i = 0;
                C4Error error;
                string buf;
                while(Native.c4queryenum_next(e, &error)) {
                    ++i;
                    if(i <= 100) {
                        buf = i.ToString();
                        e->docSequence.Should().Be(i, "because the sequences should be ordered");
                    } else {
                        buf = $"\"doc-{i-100:D3}\"";
                        e->docSequence.Should().Be(i - 100, "because the sequence number should be ordered");
                    }

                    Native.c4key_toJSON(&e->key).Should().Be(buf, "because the key on the enumerator should be correct");
                    e->value.Equals(C4Slice.Constant("1234")).Should().BeTrue("because the value should be correct");
                }

                Native.c4queryenum_free(e);
                error.Code.Should().Be(0, "because otherwise an error occurred somewhere");
                i.Should().Be(200, "because all index entries should be covered");
            });
        }

        [Fact]
        public void TestReduceAll()
        {
            RunTestVariants(() => {
                CreateIndex();

                var options = C4QueryOptions.Default;  
                var context = new CountContext();
                var reduce = new C4ManagedReduceFunction(CountAccumulate, CountReduce, context);
                var reduceNative = reduce.Native;
                options.reduce = &reduceNative;

                var e = (C4QueryEnumerator *)LiteCoreBridge.Check(err => {
                    var localOpts = options;
                    return Native.c4view_query(_view, &localOpts, err);
                });

                // First row:
                C4Error error;
                Native.c4queryenum_next(e, &error).Should().BeTrue("because otherwise the query failed");
                var valueStr = e->value.CreateString();
                Console.WriteLine($"Key: {Native.c4key_toJSON(&e->key)} Value: {valueStr}");
                Native.c4key_toJSON(&e->key).Should().BeNull("because the reduce has no key");
                valueStr.Should().Be("200", "because the reduce function should return the proper value");

                // No more rows:
                Native.c4queryenum_next(e, &error).Should().BeFalse("because the reduce function only contains one row");
                Native.c4queryenum_free(e);
                error.Code.Should().Be(0, "because otherwise an error occurred somewhere");
;            });
        }

        [Theory]
        [InlineData(true)]
        [InlineData(false)]
        public void TestDocPurge(bool compact)
        {
            RunTestVariants(() => {
                CreateIndex();
                var lastIndexed = Native.c4view_getLastSequenceIndexed(_view);
                var lastSeq = Native.c4db_getLastSequence(_db);
                lastIndexed.Should().Be(lastSeq, "because the view should be up to date");

                // Purge one of the indexed docs:
                LiteCoreBridge.Check(err => Native.c4db_beginTransaction(_db, err));
                try {
                    LiteCoreBridge.Check(err => Native.c4db_purgeDoc(_db, "doc-023", err));
                } finally {
                    LiteCoreBridge.Check(err => Native.c4db_endTransaction(_db, true, err));
                }

                if(compact) {
                    LiteCoreBridge.Check(err => Native.c4db_compact(_db, err));
                }

                // ForestDB assigns sequences to deletions, so the purge bumped the db's sequence
                // invalidating the view index:
                lastIndexed = Native.c4view_getLastSequenceIndexed(_view);
                lastSeq = Native.c4db_getLastSequence(_db);
                lastIndexed.Should().BeLessThan(lastSeq, "because the new deletion was not yet indexed");

                UpdateIndex();

                // Verify that the purged doc is no longer in the index:
                var e = (C4QueryEnumerator *)LiteCoreBridge.Check(err => Native.c4view_query(_view, null, err));
                int i = 0;
                C4Error error;
                while(Native.c4queryenum_next(e, &error)) {
                    ++i;
                }

                Native.c4queryenum_free(e);
                i.Should().Be(198, "because two rows related to doc-023 should be gone");
            });
        }

        [Fact]
        public void TestCreateFullTextIndex()
        {
            RunTestVariants(() => {
                CreateFullTextIndex(100);
            });
        }

        [Fact]
        public void TestQueryFullTextIndex()
        {
            RunTestVariants(() => {
                CreateFullTextIndex(3);

                // Search for "somewhere":
                var e = (C4QueryEnumerator *)LiteCoreBridge.Check(err => Native.c4view_fullTextQuery(_view, 
                    "somewhere", null, null, err));
                
                C4Error error;
                Native.c4queryenum_next(e, &error).Should().BeTrue("because otherwise the full text query failed");
                e->docID.Equals(C4Slice.Constant("doc-001")).Should().BeTrue("because doc-001 contains the text");
                e->docSequence.Should().Be(1, "because the enumerator should have the correct sequence for the current doc");
                e->fullTextTermCount.Should().Be(1, "because the full text information should be correct");
                e->fullTextTerms[0].termIndex.Should().Be(0, "because the full text information should be correct");
                e->fullTextTerms[0].start.Should().Be(8, "because the full text information should be correct");
                e->fullTextTerms[0].length.Should().Be(9, "because the full text information should be correct");

                Native.c4queryenum_next(e, &error).Should().BeFalse("beacuse the query only has one row");
                error.Code.Should().Be(0, "because otherwise an error occurred somewhere");
                Native.c4queryenum_free(e);

                // Search for "cat":
                e = (C4QueryEnumerator *)LiteCoreBridge.Check(err => Native.c4view_fullTextQuery(_view, 
                    "cat", null, null, err));
                int i = 0;
                while(Native.c4queryenum_next(e, &error)) {
                    ++i;
                    e->fullTextTermCount.Should().Be(1, "because the full text information should be correct");
                    e->fullTextTerms[0].termIndex.Should().Be(0, "because the full text information should be correct");
                    if(e->docSequence == 1) {
                        e->fullTextTerms[0].start.Should().Be(20, "because the full text information should be correct");
                        e->fullTextTerms[0].length.Should().Be(4, "because the full text information should be correct");
                    } else {
                        e->docSequence.Should().Be(3, "because the correct document should be indexed");
                        e->fullTextTerms[0].start.Should().Be(4, "because the full text information should be correct");
                        e->fullTextTerms[0].length.Should().Be(3, "because the full text information should be correct");
                    }
                }

                Native.c4queryenum_free(e);
                error.Code.Should().Be(0, "because otherwise an error occurred somewhere");
                i.Should().Be(2, "because there are two documents valid for the query");

                // Search for "cat bark":
                e = (C4QueryEnumerator *)LiteCoreBridge.Check(err => Native.c4view_fullTextQuery(_view, 
                    "cat bark", null, null, err));
                Native.c4queryenum_next(e, &error).Should().BeTrue("because otherwise the full text query failed");
                e->docID.Equals(C4Slice.Constant("doc-001")).Should().BeTrue("because doc-001 contains the text");
                e->docSequence.Should().Be(1, "because the enumerator should have the correct sequence for the current doc");
                e->fullTextTermCount.Should().Be(2, "because the full text information should be correct");
                e->fullTextTerms[0].termIndex.Should().Be(0, "because the full text information should be correct");
                e->fullTextTerms[0].start.Should().Be(20, "because the full text information should be correct");
                e->fullTextTerms[0].length.Should().Be(4, "because the full text information should be correct");
                e->fullTextTerms[1].termIndex.Should().Be(1, "because the full text information should be correct");
                e->fullTextTerms[1].start.Should().Be(29, "because the full text information should be correct");
                e->fullTextTerms[1].length.Should().Be(7, "because the full text information should be correct");

                Native.c4queryenum_next(e, &error).Should().BeFalse("beacuse the query only has one row");
                error.Code.Should().Be(0, "because otherwise an error occurred somewhere");
                Native.c4queryenum_free(e);
            });
        }

        private void CreateFullTextIndex(uint docCount)
        {
            for(uint i = 1; i <= docCount; i++) {
                    string docID = $"doc-{i:D3}";
                    var body = C4Slice.Null;
                    switch(i % 3) {
                        case 0:
                        body = C4Slice.Constant("The cat sat on the mat");
                        break;
                        case 1:
                        body = C4Slice.Constant("Outside SomeWhere a c\u00e4t was barking");
                        break;
                        case 2:
                        body = C4Slice.Constant("The bark of a tree is rough?");
                        break;
                    }

                    CreateRev(docID, RevID, body);
                }

                var ind = (C4Indexer *)LiteCoreBridge.Check(err => Native.c4indexer_begin(_db, 
                    new[] { _view }, err));
                var e = (C4DocEnumerator *)LiteCoreBridge.Check(err => Native.c4indexer_enumerateDocuments(ind, err));

                C4Document* doc;
                C4Error error;
                while(null != (doc = Native.c4enum_nextDocument(e, &error))) {
                    // Index 'doc':
                    var keys = new C4Key*[1];
                    var values = new C4Slice[1];
                    keys[0] = NativeRaw.c4key_newFullTextString(doc->selectedRev.body, C4Slice.Constant("en"));
                    values[0] = C4Slice.Constant("1234");
                    LiteCoreBridge.Check(err => Native.c4indexer_emit(ind, doc, 0, keys, values, err));
                    Native.c4key_free(keys[0]);
                    Native.c4doc_free(doc);
                }

                error.Code.Should().Be(0, "because otherwise an error occurred somewhere");
                Native.c4enum_free(e);
                LiteCoreBridge.Check(err => Native.c4indexer_end(ind, true, err));
        }

        private void CreateIndex()
        {
            for(int i = 1; i <= 100; i++) {
                var docID = $"doc-{i:D3}";
                CreateRev(docID, RevID, Body);
            }

            UpdateIndex();
        }

        private void UpdateIndex()
        {
            var ind = (C4Indexer *)LiteCoreBridge.Check(err => Native.c4indexer_begin(_db, 
                new[] { _view }, err));
            var e = (C4DocEnumerator *)LiteCoreBridge.Check(err => Native.c4indexer_enumerateDocuments(ind, err));
            C4Document* doc;
            C4Error error;
            while(null != (doc = Native.c4enum_nextDocument(e, &error))) {
                // Index 'doc':
                var keys = new C4Key*[2];
                var values = new C4Slice[2];
                keys[0] = Native.c4key_new();
                keys[1] = Native.c4key_new();
                NativeRaw.c4key_addString(keys[0], doc->docID);
                Native.c4key_addNumber(keys[1], doc->sequence);
                values[0] = values[1] = C4Slice.Constant("1234");
                LiteCoreBridge.Check(err => Native.c4indexer_emit(ind, doc, 0, keys, values, err));
                Native.c4key_free(keys[0]);
                Native.c4key_free(keys[1]);
                Native.c4doc_free(doc);
            }

            error.Code.Should().Be(0, "because otherwise an error occurred somewhere");
            Native.c4enum_free(e);
            LiteCoreBridge.Check(err => Native.c4indexer_end(ind, true, err));
        }

        private static void CountAccumulate(object context, C4Key* key, C4Slice value)
        {
            (context as CountContext).count++;
        }

        private static string CountReduce(object context)
        {
            var ctx = context as CountContext;
            var retVal = ctx.count.ToString();
            ctx.count = 0;
            return retVal;
        }

        protected override void SetupVariant(int options)
        {
            Native.c4view_deleteByName(_db, "myview", null);
            _view = (C4View *)LiteCoreBridge.Check(err => Native.c4view_open(_db, null, "myview", "1",
                Native.c4db_getConfig(_db), err));
        }

        protected override void TeardownVariant(int options)
        {
            if(_view != null) {
                LiteCoreBridge.Check(err => Native.c4view_delete(_view, err));
                Native.c4view_free(_view);
            }
        }
    }
}