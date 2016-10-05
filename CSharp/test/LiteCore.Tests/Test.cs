using System;
using System.IO;
using System.Text;

using FluentAssertions;
using LiteCore.Interop;

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

        private void Log(C4LogLevel level, C4Slice s)
        {
            Console.WriteLine($"[{level}] {s.CreateString()}");
        }

        private void OpenDatabase(int options)
        {
            _storage = (options & 1) != 0 ? "ForestDB" : "SQLite";
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
            var bytes = Encoding.ASCII.GetBytes(_storage);
            fixed(byte* b = bytes) {
                config.storageEngine = b;
                Native.c4db_deleteAtPath(databasePath(), &config, null);
                _db = Native.c4db_open(databasePath(), &config, &err);
            }
            ((long)_db).Should().NotBe(0, "because otherwise the database failed to open");
        }

        private void CloseAndDelete(int options)
        {
            LiteCoreBridge.Check(err => Native.c4db_delete(_db, err));
            Native.c4db_free(_db);
            _db = null;
        }

        private string databasePath()
        {
            if(_bundled) {
                return Path.Combine(TestDir, "cbl_core_test");
            } else if(_storage == C4StorageEngine.SQLite) {
                return Path.Combine(TestDir, "cbl_core_test.sqlite3");
            } else {
                return Path.Combine(TestDir, "cbl_core_test.forestdb");
            }
        }
    }
}
