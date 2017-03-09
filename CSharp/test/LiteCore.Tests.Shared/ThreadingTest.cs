using System;
using System.Threading.Tasks;
using FluentAssertions;
using LiteCore.Interop;
using Xunit;
using Xunit.Abstractions;
using System.Threading;

namespace LiteCore.Tests
{
    public unsafe class ThreadingTest : Test
    {
        private bool Log = false;
        private const int NumDocs = 10000;
        private const bool SharedHandle = false; // Use same C4Database on all threads_
        private object _observerMutex = new object();
        private bool _changesToObserve;

        public ThreadingTest(ITestOutputHelper output) : base(output)
        {

        }

        [Fact]
        public void TestCreateVsEnumerate()
        {
            RunTestVariants(() => {
                var task1 = Task.Factory.StartNew(AddDocsTask);
                var task2 = Task.Factory.StartNew(ObserverTask);

                Task.WaitAll(task1, task2);
            });
        }

        private void AddDocsTask()
        {
            // This implicitly uses the 'db' connection created (but not used) by the main thread
            if(Log) {
                WriteLine("Adding documents...");
            }

            for(int i = 1; i <= NumDocs; i++) {
                if(Log) {
                    Write($"({i}) ");
                } else if(i%10 == 0) {
                    Write(":");
                }

                var docID = $"doc-{i:D5}";
                CreateRev(docID, RevID, Body);
            }
            WriteLine();
        }

        private void ObserverTask()
        {
            var database = OpenDB();
            var observer = Native.c4dbobs_create(database, ObsCallback, this);
            var lastSequence = 0UL;
            do {
                lock (_observerMutex) {
                    if (!_changesToObserve) {
                        continue;
                    }

                    Write("8");
                    _changesToObserve = false;
                }

                var changes = new C4DatabaseChange[10];
                uint nDocs;
                bool external;
                while (0 < (nDocs = Native.c4dbobs_getChanges(observer.Observer, changes, 10U, &external))) {
                    external.Should().BeTrue("because all changes will be external in this test");
                    for (int i = 0; i < nDocs; ++i) {
                        changes[i].docID.CreateString().Should().StartWith("doc-", "because otherwise the document ID is not what we created");
                        lastSequence = changes[i].sequence;
                    }
                }

                Thread.Sleep(TimeSpan.FromMilliseconds(100));
            } while (lastSequence < NumDocs);

            observer.Dispose();
            CloseDB(database);
        }

        private static void ObsCallback(C4DatabaseObserver* observer, object context)
        {
            ((ThreadingTest)context).Observe(observer);
        }

        private void Observe(C4DatabaseObserver* observer)
        {
            Write("!");
            lock(_observerMutex) {
                _changesToObserve = true;
            }
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
    }
}