using System.Collections.Generic;
using FluentAssertions;
using LiteCore.Interop;
using Xunit;

namespace LiteCore.Tests
{
    public unsafe class ObserverTest : Test
    {
        private DatabaseObserver _dbObserver;
        private DocumentObserver _docObserver;

        private int _dbCallbackCalls;

        private int _docCallbackCalls;

        protected override int NumberOfOptions 
        {
            get {
                return 1;
            }
        }

        [Fact]
        public void TestDBObserver()
        {
            RunTestVariants(() => {
                _dbObserver = Native.c4dbobs_create(Db, DBObserverCallback, this);
                CreateRev("A", C4Slice.Constant("1-aa"), Body);
                _dbCallbackCalls.Should().Be(1, "because we should have received a callback");
                CreateRev("B", C4Slice.Constant("1-bb"), Body);
                _dbCallbackCalls.Should().Be(1, "because we should have received a callback");

                CheckChanges(new[] { "A", "B" });

                CreateRev("B", C4Slice.Constant("2-bbbb"), Body);
                _dbCallbackCalls.Should().Be(2, "because we should have received a callback");
                CreateRev("C", C4Slice.Constant("1-cc"), Body);
                _dbCallbackCalls.Should().Be(2, "because we should have received a callback");

                 CheckChanges(new[] { "B", "C" });
                 _dbObserver.Dispose();
                 _dbObserver = null;

                 CreateRev("A", C4Slice.Constant("2-aaaa"), Body);
                 _dbCallbackCalls.Should().Be(2, "because the observer was disposed");
            });
        }

        [Fact]
        public void TestDocObserver()
        {
            RunTestVariants(() => {
                CreateRev("A", C4Slice.Constant("1-aa"), Body);
                _docObserver = Native.c4docobs_create(Db, "A", DocObserverCallback, this);
                
                CreateRev("A", C4Slice.Constant("2-bb"), Body);
                CreateRev("B", C4Slice.Constant("1-bb"), Body);
                _docCallbackCalls.Should().Be(1, "because there was only one update to the doc in question");
            });
        }

        [Fact]
        public void TestMultiDbObserver()
        {
            RunTestVariants(() => {
                _dbObserver = Native.c4dbobs_create(Db, DBObserverCallback, this);
                CreateRev("A", C4Slice.Constant("1-aa"), Body);
                _dbCallbackCalls.Should().Be(1, "because we should have received a callback");
                CreateRev("B", C4Slice.Constant("1-bb"), Body);
                _dbCallbackCalls.Should().Be(1, "because we should have received a callback");

                CheckChanges(new[] { "A", "B" });

                // Open another database on the same file
                var otherdb = (C4Database *)LiteCoreBridge.Check(err => Native.c4db_open(DatabasePath(), Native.c4db_getConfig(Db), err));
                LiteCoreBridge.Check(err => Native.c4db_beginTransaction(otherdb, err));
                try {
                    CreateRev(otherdb, "C", C4Slice.Constant("1-cc"), Body);
                    CreateRev(otherdb, "D", C4Slice.Constant("1-dd"), Body);
                    CreateRev(otherdb, "E", C4Slice.Constant("1-ee"), Body);
                } finally {
                    LiteCoreBridge.Check(err => Native.c4db_endTransaction(otherdb, true, err));
                }

                _dbCallbackCalls.Should().Be(2, "because the observer should cover all connections");

                CheckChanges(new[] { "C", "D", "E" }, true);
                _dbObserver.Dispose();
                _dbObserver = null;

                CreateRev("A", C4Slice.Constant("2-aaaa"), Body);
                _dbCallbackCalls.Should().Be(2, "because the observer was disposed");

                LiteCoreBridge.Check(err => Native.c4db_close(otherdb, err));
                Native.c4db_free(otherdb);
            });
        }

        private void CheckChanges(IList<string> expectedDocIDs, bool expectedExternal = false)
        {
            var docIDs = new string[100];
            ulong lastSeq;
            bool external;
            var changeCount = Native.c4dbobs_getChanges(_dbObserver.Observer, docIDs, &lastSeq, &external);
            changeCount.Should().Be((uint)expectedDocIDs.Count, "because otherwise we didn't get the correct number of changes");
            int i = 0;
            foreach(var docID in expectedDocIDs) {
                docIDs[i++].Should().Be(docID, "because otherwise we have an invalid document");
            }

            external.Should().Be(expectedExternal, "because otherwise the external parameter was wrong");
        }

        private static void DBObserverCallback(C4DatabaseObserver* obs, object context)
        {
            ((ObserverTest)context).DbObserverCalled(obs);
        }

        private static void DocObserverCallback(C4DocumentObserver* obs, string docID, ulong sequence, object context)
        {
            ((ObserverTest)context).DocObserverCalled(obs, docID, sequence);
        }

        private void DbObserverCalled(C4DatabaseObserver *obs)
        {
            ((long)obs).Should().Be((long)_dbObserver.Observer, "because the callback should be for the proper DB");
            _dbCallbackCalls++;
        }

        private void DocObserverCalled(C4DocumentObserver *obs, string docID, ulong sequence)
        {
            ((long)obs).Should().Be((long)_docObserver.Observer, "because the callback should be for the proper DB");
            _docCallbackCalls++;
        }

        protected override void SetupVariant(int option)
        {
            base.SetupVariant(option);

            _dbCallbackCalls = 0;
            _docCallbackCalls = 0;
        }

        protected override void TeardownVariant(int option)
        {
            _dbObserver?.Dispose();
            _dbObserver = null;
            _docObserver?.Dispose();
            _docObserver = null;

            base.TeardownVariant(option);
        }
    }
}