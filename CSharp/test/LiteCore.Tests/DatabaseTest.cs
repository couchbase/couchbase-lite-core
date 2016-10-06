using System.IO;
using FluentAssertions;
using LiteCore.Interop;
using Xunit;

namespace LiteCore.Tests
{
    public unsafe class DatabaseTest : Test
    {
        [Fact]
        public void TestErrorMessages()
        {
            var msg = Native.c4error_getMessage(new C4Error(C4ErrorDomain.ForestDB, 0));
            msg.Should().BeNull("because there was no error");

            AssertMessage(C4ErrorDomain.ForestDB, (int)ForestDBStatus.KeyNotFound, "key not found");
            AssertMessage(C4ErrorDomain.SQLite, (int)SQLiteStatus.Corrupt, "database disk image is malformed");
            AssertMessage(C4ErrorDomain.LiteCore, (int)LiteCoreError.InvalidParameter, "invalid parameter");
            AssertMessage(C4ErrorDomain.POSIX, (int)PosixStatus.NOENT, "No such file or directory");
            AssertMessage(C4ErrorDomain.LiteCore, (int)LiteCoreError.IndexBusy, "index busy; can't close view");
            AssertMessage(C4ErrorDomain.ForestDB, -1234, "unknown error");
            AssertMessage((C4ErrorDomain)666, -1234, "unknown error domain");
        }

        [Fact]
        public void TestOpenBundle()
        {
            RunTestVariants(() => {
                var config = C4DatabaseConfig.Clone(Native.c4db_getConfig(_db));
                config.flags |= C4DatabaseFlags.Bundled;
                var tmp = config;

                var bundlePath = Path.Combine(TestDir, "cbl_core_test_bundle") + "/";
                Native.c4db_deleteAtPath(bundlePath, &config, null);
                var bundle = (C4Database *)LiteCoreBridge.Check(err => {
                    var localConfig = tmp;
                    return Native.c4db_open(bundlePath, &localConfig, err);
                });

                var path = Native.c4db_getPath(bundle);
                path.Should().Be(bundlePath, "because the database should store the correct path");
                LiteCoreBridge.Check(err => Native.c4db_close(bundle, err));
                Native.c4db_free(bundle);

                // Reopen without the 'create' flag:
                config.flags &= ~C4DatabaseFlags.Create;
                tmp = config;
                bundle = (C4Database *)LiteCoreBridge.Check(err => {
                    var localConfig = tmp;
                    return Native.c4db_open(bundlePath, &localConfig, err);
                });
                LiteCoreBridge.Check(err => Native.c4db_close(bundle, err));
                Native.c4db_free(bundle);

                // Reopen with wrong storage type:
                Native.c4log_warnOnErrors(false);
                if(config.storageEngine == C4StorageEngine.SQLite) {
                    config.storageEngine = C4StorageEngine.ForestDB;
                } else {
                    config.storageEngine = C4StorageEngine.SQLite;
                }

                C4Error error;
                ((long)Native.c4db_open(bundlePath, &config, &error)).Should().Be(0, 
                    "because the storage engine is invalid");

                ((long)Native.c4db_open(Path.Combine(TestDir, "no_such_bundle"), &config, &error)).Should().Be(0, 
                    "because the storage engine is invalid");
                Native.c4log_warnOnErrors(true);
                config.Dispose();
            });
        }

        [Fact]
        public void TestTransaction()
        {
            RunTestVariants(() => {
                Native.c4db_getDocumentCount(_db).Should().Be(0, "because no documents have been added");
                Native.c4db_isInTransaction(_db).Should().BeFalse("because no transaction has started yet");
                LiteCoreBridge.Check(err => Native.c4db_beginTransaction(_db, err));
                Native.c4db_isInTransaction(_db).Should().BeTrue("because a transaction has started");
                LiteCoreBridge.Check(err => Native.c4db_beginTransaction(_db, err));
                Native.c4db_isInTransaction(_db).Should().BeTrue("because another transaction has started");
                LiteCoreBridge.Check(err => Native.c4db_endTransaction(_db, true, err));
                Native.c4db_isInTransaction(_db).Should().BeTrue("because a transaction is still active");
                LiteCoreBridge.Check(err => Native.c4db_endTransaction(_db, true, err));
                Native.c4db_isInTransaction(_db).Should().BeFalse("because all transactions have ended");
            });
        }

        [Fact]
        public void TestCreateRawDoc()
        {
            RunTestVariants(() => {
                var key = C4Slice.Constant("key");
                var meta = C4Slice.Constant("meta");
                LiteCoreBridge.Check(err => Native.c4db_beginTransaction(_db, err));
                LiteCoreBridge.Check(err => NativeRaw.c4raw_put(_db, C4Slice.Constant("test"), key, meta, 
                    Body, err));
                LiteCoreBridge.Check(err => Native.c4db_endTransaction(_db, true, err));

                var doc = (C4RawDocument *)LiteCoreBridge.Check(err => NativeRaw.c4raw_get(_db,
                    C4Slice.Constant("test"), key, err));
                doc->key.Equals(key).Should().BeTrue("because the key should not change");
                doc->meta.Equals(meta).Should().BeTrue("because the meta should not change");
                doc->body.Equals(Body).Should().BeTrue("because the body should not change");
                Native.c4raw_free(doc);

                // Nonexistent:
                C4Error error;
                ((long)Native.c4raw_get(_db, "test", "bogus", &error)).Should().Be(0, 
                    "because the document does not exist");
                error.Domain.Should().Be(C4ErrorDomain.LiteCore, "because that is the correct domain");
                error.Code.Should().Be((int)LiteCoreError.NotFound, "because that is the correct error code");
            });
        }

        [Fact]
        public void TestCreateVersionedDoc()
        {
            RunTestVariants(() => {
                // Try reading doc with mustExist=true, which should fail:
                C4Error error;
                C4Document* doc = NativeRaw.c4doc_get(_db, DocID, true, &error);
                ((long)doc).Should().Be(0, "because the document does not exist");
                error.Domain.Should().Be(C4ErrorDomain.LiteCore);
                error.Code.Should().Be((int)LiteCoreError.NotFound);
                Native.c4doc_free(doc);

                // Now get the doc with mustExist=false, which returns an empty doc:
                doc = (C4Document *)LiteCoreBridge.Check(err => NativeRaw.c4doc_get(_db, DocID, false, err));
                ((int)doc->flags).Should().Be(0, "because the document is empty");
                doc->docID.Equals(DocID).Should().BeTrue("because the doc ID should match what was stored");
                ((long)doc->revID.buf).Should().Be(0, "because the doc has no revision ID yet");
                ((long)doc->selectedRev.revID.buf).Should().Be(0, "because the doc has no revision ID yet");
                Native.c4doc_free(doc);

                LiteCoreBridge.Check(err => Native.c4db_beginTransaction(_db, err));
                try {
                    var tmp = new[] { RevID };
                    fixed(C4Slice* history = tmp) {
                        var rq = new C4DocPutRequest {
                            existingRevision = true,
                            docID = DocID,
                            history = history,
                            historyCount = 1,
                            body = Body,
                            save = true
                        };

                        doc = (C4Document *)LiteCoreBridge.Check(err => {
                            var localRq = rq;
                            return Native.c4doc_put(_db, &localRq, null, err);
                        });
                        doc->revID.Equals(RevID).Should().BeTrue("because the doc should have the stored revID");
                        doc->selectedRev.revID.Equals(RevID).Should().BeTrue("because the doc should have the stored revID");
                        doc->selectedRev.flags.Should().Be(C4RevisionFlags.Leaf, "because this is a leaf revision");
                        doc->selectedRev.body.Equals(Body).Should().BeTrue("because the body should be stored correctly");
                        Native.c4doc_free(doc);
                    }
                } finally {
                    LiteCoreBridge.Check(err => Native.c4db_endTransaction(_db, true, err));
                }

                // Reload the doc:
                doc = (C4Document *)LiteCoreBridge.Check(err => NativeRaw.c4doc_get(_db, DocID, true, err));
                doc->flags.Should().Be(C4DocumentFlags.Exists, "because this is an existing document");
                doc->docID.Equals(DocID).Should().BeTrue("because the doc should have the stored doc ID");
                doc->revID.Equals(RevID).Should().BeTrue("because the doc should have the stored rev ID");
                doc->selectedRev.revID.Equals(RevID).Should().BeTrue("because the doc should have the stored rev ID");
                doc->selectedRev.sequence.Should().Be(1, "because it is the first stored document");
                doc->selectedRev.body.Equals(Body).Should().BeTrue("because the doc should have the stored body");
                Native.c4doc_free(doc);

                // Get the doc by its sequence
                doc = (C4Document *)LiteCoreBridge.Check(err => Native.c4doc_getBySequence(_db, 1, err));
                doc->flags.Should().Be(C4DocumentFlags.Exists, "because this is an existing document");
                doc->docID.Equals(DocID).Should().BeTrue("because the doc should have the stored doc ID");
                doc->revID.Equals(RevID).Should().BeTrue("because the doc should have the stored rev ID");
                doc->selectedRev.revID.Equals(RevID).Should().BeTrue("because the doc should have the stored rev ID");
                doc->selectedRev.sequence.Should().Be(1, "because it is the first stored document");
                doc->selectedRev.body.Equals(Body).Should().BeTrue("because the doc should have the stored body");
                Native.c4doc_free(doc);
            });
        }

        private void AssertMessage(C4ErrorDomain domain, int code, string expected)
        {
            var msg = Native.c4error_getMessage(new C4Error(domain, code));
            msg.Should().Be(expected, "because the error message should match the code");
        }
    }
}