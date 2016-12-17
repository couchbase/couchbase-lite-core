using System;
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

                _view = (C4View *)LiteCoreBridge.Check(err => Native.c4view_open(Db, null, "myview", "1",
                    Native.c4db_getConfig(Db), err));
                Native.c4view_getTotalRows(_view).Should().Be(200, "because the view information should survive reopening");
                Native.c4view_getLastSequenceIndexed(_view).Should().Be(100, "because the view information should survive reopening");
                Native.c4view_getLastSequenceChangedAt(_view).Should().Be(100, "because the view information should survive reopening");
                
                // Reopen view with different version string:
                LiteCoreBridge.Check(err => Native.c4view_close(_view, err));
                Native.c4view_free(_view);

                _view = (C4View *)LiteCoreBridge.Check(err => Native.c4view_open(Db, null, "myview", "2",
                    Native.c4db_getConfig(Db), err));
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

                ulong i = 0;
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
                error.code.Should().Be(0, "because otherwise an error occurred somewhere");
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
                error.code.Should().Be(0, "because otherwise an error occurred somewhere");
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
                var lastSeq = Native.c4db_getLastSequence(Db);
                lastIndexed.Should().Be(lastSeq, "because the view should be up to date");

                // Purge one of the indexed docs:
                LiteCoreBridge.Check(err => Native.c4db_beginTransaction(Db, err));
                try {
                    LiteCoreBridge.Check(err => Native.c4db_purgeDoc(Db, "doc-023", err));
                } finally {
                    LiteCoreBridge.Check(err => Native.c4db_endTransaction(Db, true, err));
                }

                if(compact) {
                    LiteCoreBridge.Check(err => Native.c4db_compact(Db, err));
                }

                // ForestDB assigns sequences to deletions, so the purge bumped the db's sequence
                // invalidating the view index:
                lastIndexed = Native.c4view_getLastSequenceIndexed(_view);
                lastSeq = Native.c4db_getLastSequence(Db);
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
            var ind = (C4Indexer *)LiteCoreBridge.Check(err => Native.c4indexer_begin(Db, 
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
                LiteCoreBridge.Check(err => NativeRaw.c4indexer_emit(ind, doc, 0, keys, values, err));
                Native.c4key_free(keys[0]);
                Native.c4key_free(keys[1]);
                Native.c4doc_free(doc);
            }

            error.code.Should().Be(0, "because otherwise an error occurred somewhere");
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
            base.SetupVariant(options);

            Native.c4view_deleteByName(Db, "myview", null);
            _view = (C4View *)LiteCoreBridge.Check(err => Native.c4view_open(Db, null, "myview", "1",
                Native.c4db_getConfig(Db), err));
        }

        protected override void TeardownVariant(int options)
        {
            if(_view != null) {
                LiteCoreBridge.Check(err => Native.c4view_delete(_view, err));
                Native.c4view_free(_view);
            }

            base.TeardownVariant(options);
        }
    }
}