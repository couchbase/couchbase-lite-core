using System;
using System.IO;

using FluentAssertions;
using LiteCore;
using LiteCore.Interop;
using Xunit;


namespace Tests
{
    public unsafe class Test
    {
        private const string TestDir = "/tmp";
        public static readonly int NumberOfOptions = 4;

        private C4DocumentVersioning _versioning;
        private string _storage;

        protected C4Database* _db { get; private set; }

        protected void RunTestVariants(Action a)
        {
            for(int i = 0; i < 4; i++) {
                OpenDatabase(i);
                SetupVariant(i);
                try {
                    a();
                } finally {
                    CloseAndDelete();
                }
            }
        }

        protected bool isRevTrees()
        {
            return _versioning == C4DocumentVersioning.RevisionTrees;
        }

        protected virtual void SetupVariant(int options)
        {
            
        }

        private void OpenDatabase(int options)
        {
            _storage = (options & 1) != 0 ? "ForestDB" : "SQLite";
            _versioning = (options & 2) != 0 ? C4DocumentVersioning.VersionVectors : C4DocumentVersioning.RevisionTrees;
            Native.c4_shutdown(null);

            var config = new C4DatabaseConfig();
            config.flags = C4DatabaseFlags.Create;
            config.versioning = _versioning;

            if(_versioning == C4DocumentVersioning.RevisionTrees) {

            } else {

            }

            C4Error err;
            Native.c4db_deleteAtPath(databasePath(), &config, null);
            _db = Native.c4db_open(databasePath(), &config, &err);
            ((long)_db).Should().NotBe(0, "because otherwise the database failed to open");
        }

        private void CloseAndDelete()
        {
            LiteCoreBridge.Check(err => Native.c4db_delete(_db, err));
            Native.c4db_free(_db);
            _db = null;
        }

        private string databasePath()
        {
            if(_storage == "SQLite") {
                return Path.Combine(TestDir, "cbl_core_test.sqlite3");
            } else {
                return Path.Combine(TestDir, "cbl_core_test.forestdb");
            }
        }
    }
}
