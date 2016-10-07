using System;
using System.IO;
using System.Threading.Tasks;
using FluentAssertions;
using LiteCore.Interop;
using Xunit;

namespace LiteCore.Tests
{
    public unsafe class ThreadingTest : Test
    {
        private const bool Log = false;
        private const int NumDocs = 10000;
        private const bool SharedHandle = false; // Use same C4Database on all threads_

        private C4View* _view;

        [Fact]
        public void TestCreateVsEnumerate()
        {
            RunTestVariants(() => {
                var task1 = Task.Factory.StartNew(AddDocsTask);
                var task2 = Task.Factory.StartNew(UpdateIndexTask);
                var task3 = Task.Factory.StartNew(QueryIndexTask);

                Task.WaitAll(task1, task2, task3);
            });
        }

        private void AddDocsTask()
        {
            // This implicitly uses the 'db' connection created (but not used) by the main thread
            if(Log) {
                Console.WriteLine("Adding documents...");
            }

            for(int i = 1; i <= NumDocs; i++) {
                if(Log) {
                    Console.Write($"({i}) ");
                } else if(i%10 == 0) {
                    Console.Write(":");
                }

                var docID = $"doc-{i:D5}";
                CreateRev(docID, RevID, Body);
            }
        }

        private void UpdateIndexTask()
        {
            var database = SharedHandle ? Db : OpenDB();
            var view = SharedHandle ? _view : OpenView(database);

            int i = 0;
            do {
                if(Log) {
                    Console.WriteLine();
                    Console.Write($"Index update #{++i:D3}");
                }

                UpdateIndex(database, view);
                Task.Delay(TimeSpan.FromTicks(700 * (TimeSpan.TicksPerMillisecond / 1000))).Wait();
            } while(Native.c4view_getLastSequenceIndexed(view) < NumDocs);

            if(!SharedHandle) {
                CloseView(view);
                CloseDB(database);
            }
        }

        private void QueryIndexTask()
        {
            var database = SharedHandle ? Db : OpenDB();
            var view = SharedHandle ? _view : OpenView(database);

            int i = 0;
            do {
                Task.Delay(1).Wait();
                if(Log) {
                    Console.WriteLine();
                    Console.Write($"Index query #{++i:D3}");
                }
            } while(QueryIndex(view));

            if(!SharedHandle) {
                CloseView(view);
                CloseDB(database);
            }
        }

        private bool QueryIndex(C4View* view)
        {
            var e = (C4QueryEnumerator *)LiteCoreBridge.Check(err => Native.c4view_query(view, null, err));
            if(Log) {
                Console.Write("{ ");
            }

            ulong i = 0;
            C4Error error;
            while(Native.c4queryenum_next(e, &error)) {
                ++i;
                var buf = $"\"doc-{i:D5}\"";
                if(e->docSequence != i) {
                    if(Log) {
                        var gotID = e->docID.CreateString();
                        Console.WriteLine();
                        Console.WriteLine($"*** Expected {buf}, got {gotID} ***");
                    }

                    i = e->docSequence;
                    continue;
                }

                Native.c4key_toJSON(&e->key).Should().Be(buf, "because the docID should be correct");
                e->value.Equals(C4Slice.Constant("1234")).Should().BeTrue("because the value should be accurate");
            }

            if(Log) {
                Console.Write($"}}queried_to:{i}");
            }

            Native.c4queryenum_free(e);
            error.Code.Should().Be(0, "because otherwise an error occurred somewhere");
            return i < NumDocs;
        }

        private void UpdateIndex(C4Database* updateDB, C4View* view)
        {
            var oldLastSeqIndexed = Native.c4view_getLastSequenceIndexed(view);
            var lastSeq = oldLastSeqIndexed;
            var ind = (C4Indexer *)LiteCoreBridge.Check(err => Native.c4indexer_begin(updateDB, 
            new[] { view }, err));
            var e = (C4DocEnumerator *)LiteCoreBridge.Check(err => Native.c4indexer_enumerateDocuments(ind, err));
            if(e == null) {
                LiteCoreBridge.Check(err => Native.c4indexer_end(ind, true, err));
                return;
            }

            if(Log) {
                Console.Write("<< ");
            }

            C4Document* doc;
            C4Error error;
            while(null != (doc = Native.c4enum_nextDocument(e, &error))) {
                // Index 'doc':
                if(Log) {
                    Console.Write($"(#{doc->sequence}) ");
                }

                if(lastSeq > 0) {
                    doc->sequence.Should().Be(lastSeq+1, "because the sequences should be ordered");
                }

                lastSeq = doc->sequence;
                var keys = new C4Key*[1];
                var values = new C4Slice[1];
                keys[0] = Native.c4key_new();
                NativeRaw.c4key_addString(keys[0], doc->docID);
                values[0] = C4Slice.Constant("1234");
                LiteCoreBridge.Check(err => Native.c4indexer_emit(ind, doc, 0, keys, values, err));
                Native.c4key_free(keys[0]);
                Native.c4doc_free(doc);
            }

            error.Code.Should().Be(0, "because otherwise an error occurred somewhere");
            Native.c4enum_free(e);
            if(Log) {
                Console.Write($">>indexed_to:{lastSeq} ");
            }

            LiteCoreBridge.Check(err => Native.c4indexer_end(ind, true, err));

            
            var newLastSeqIndexed = Native.c4view_getLastSequenceIndexed(view);
            if(newLastSeqIndexed != lastSeq) {
                if(Log) {
                    Console.Write($"BUT view.lastSequenceIndexed={newLastSeqIndexed}! (Started at {oldLastSeqIndexed})");
                }
            }

            newLastSeqIndexed.Should().Be(lastSeq, "because the last sequence in the loop should be current");
            Native.c4view_getLastSequenceChangedAt(view).Should().Be(lastSeq);
        }

        private C4Database* OpenDB()
        {
            var database = (C4Database *)LiteCoreBridge.Check(err => Native.c4db_open(DatabasePath(), 
                Native.c4db_getConfig(Db), err));
            return database;
        }

        private void CloseDB(C4Database* db)
        {
            Native.c4db_close(db, null);
            Native.c4db_free(db);
        }

        private C4View* OpenView(C4Database* onDB)
        {
            var view = (C4View *)LiteCoreBridge.Check(err => Native.c4view_open(onDB, null, "myview", "1",
                Native.c4db_getConfig(Db), err));
            return view;
        }

        private void CloseView(C4View* view)
        {
            Native.c4view_close(view, null);
            Native.c4view_free(view);
        }

        protected override void SetupVariant(int option)
        {
            base.SetupVariant(option);

            _view = OpenView(Db);
        }

        protected override void TeardownVariant(int option)
        {
            CloseView(_view);

            base.TeardownVariant(option);
        }
    }
}