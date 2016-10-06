using System;
using System.IO;
using System.Text;

using FluentAssertions;
using LiteCore.Interop;
using LiteCore.Util;

namespace LiteCore.Tests
{
    public unsafe class Test : TestBase
    {
        public const string TestDir = "/tmp";
        protected static readonly C4Slice Body = C4Slice.Constant("{\"name\":007}");
        
        protected override int NumberOfOptions
        {
            get {
                return 4;
            }
        }

        private C4DocumentVersioning _versioning;
        private string _storage;
        private bool _bundled = true;

        protected C4Database* _db { get; private set; }

        protected C4Slice DocID
        {
            get {
                return C4Slice.Constant("mydoc");
            }
        }

        protected C4Slice RevID
        {
            get {
                return isRevTrees() ? C4Slice.Constant("1-abcd") : C4Slice.Constant("1@*");
            }
        }

        protected C4Slice Rev2ID
        {
            get {
                return isRevTrees() ? C4Slice.Constant("2-c001d00d") : C4Slice.Constant("2@*");
            }
        }

        protected C4Slice Rev3ID
        {
            get {
                return isRevTrees() ? C4Slice.Constant("3-deadbeef") : C4Slice.Constant("3@*");
            }
        }

        public Test()
        {
            Native.c4log_setLevel(C4LogLevel.Warning);
        }

        protected bool isRevTrees()
        {
            return _versioning == C4DocumentVersioning.RevisionTrees;
        }

        protected override void SetupVariant(int option)
        {
            OpenDatabase(option);
        }

        protected override void TeardownVariant(int option)
        {
            CloseAndDelete(option);
        }

        protected void CreateRev(string docID, C4Slice revID, C4Slice body, bool isNew = true)
        {
            LiteCoreBridge.Check(err => Native.c4db_beginTransaction(_db, err));
            try {
                var curDoc = (C4Document *)LiteCoreBridge.Check(err => Native.c4doc_get(_db, docID, 
                    false, err));
                var history = new[] { revID, curDoc->revID };
                fixed(C4Slice* h = history) {
                    var rq = new C4DocPutRequest {
                        existingRevision = true,
                        docID = curDoc->docID,
                        history = h,
                        historyCount = curDoc->revID.buf != null ? 2UL : 1UL,
                        body = body,
                        deletion = body.buf == null,
                        save = true
                    };

                    var doc = (C4Document *)LiteCoreBridge.Check(err => {
                        var localRq = rq;
                        return Native.c4doc_put(_db, &localRq, null, err);
                    });
                    Native.c4doc_free(doc);
                    Native.c4doc_free(curDoc);
                }
            } finally {
                LiteCoreBridge.Check(err => Native.c4db_endTransaction(_db, true, err));
            }
        }

        protected string DatabasePath()
        {
            if(_bundled) {
                return Path.Combine(TestDir, "cbl_core_test");
            } else if(_storage == C4StorageEngine.SQLite) {
                return Path.Combine(TestDir, "cbl_core_test.sqlite3");
            } else {
                return Path.Combine(TestDir, "cbl_core_test.forestdb");
            }
        }

        private void Log(C4LogLevel level, C4Slice s)
        {
            Console.WriteLine($"[{level}] {s.CreateString()}");
        }

        private void OpenDatabase(int options)
        {
            _storage = (options & 1) != 0 ? C4StorageEngine.ForestDB : C4StorageEngine.SQLite;
            _versioning = (options & 2) != 0 ? C4DocumentVersioning.VersionVectors : C4DocumentVersioning.RevisionTrees;
            Native.c4_shutdown(null);

            var config = new C4DatabaseConfig();
            config.flags = C4DatabaseFlags.Create;
            config.versioning = _versioning;

            if(_bundled) {
                config.flags |= C4DatabaseFlags.Bundled;
            }

            Console.WriteLine($"Opening {_storage} database using {_versioning}");

            C4Error err;
            config.storageEngine = _storage;
            Native.c4db_deleteAtPath(DatabasePath(), &config, null);
            _db = Native.c4db_open(DatabasePath(), &config, &err);
            ((long)_db).Should().NotBe(0, "because otherwise the database failed to open");
        }

        private void CloseAndDelete(int options)
        {
            var config = C4DatabaseConfig.Get(Native.c4db_getConfig(_db));
            config.Dispose();
            LiteCoreBridge.Check(err => Native.c4db_delete(_db, err));
            Native.c4db_free(_db);
            _db = null;
        }
    }
}
