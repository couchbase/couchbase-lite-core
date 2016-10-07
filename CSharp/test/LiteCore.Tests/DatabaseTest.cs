using System;
using System.IO;
using System.Threading.Tasks;
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
                var config = C4DatabaseConfig.Clone(Native.c4db_getConfig(Db));
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
                Native.c4db_getDocumentCount(Db).Should().Be(0, "because no documents have been added");
                Native.c4db_isInTransaction(Db).Should().BeFalse("because no transaction has started yet");
                LiteCoreBridge.Check(err => Native.c4db_beginTransaction(Db, err));
                Native.c4db_isInTransaction(Db).Should().BeTrue("because a transaction has started");
                LiteCoreBridge.Check(err => Native.c4db_beginTransaction(Db, err));
                Native.c4db_isInTransaction(Db).Should().BeTrue("because another transaction has started");
                LiteCoreBridge.Check(err => Native.c4db_endTransaction(Db, true, err));
                Native.c4db_isInTransaction(Db).Should().BeTrue("because a transaction is still active");
                LiteCoreBridge.Check(err => Native.c4db_endTransaction(Db, true, err));
                Native.c4db_isInTransaction(Db).Should().BeFalse("because all transactions have ended");
            });
        }

        [Fact]
        public void TestCreateRawDoc()
        {
            RunTestVariants(() => {
                var key = C4Slice.Constant("key");
                var meta = C4Slice.Constant("meta");
                LiteCoreBridge.Check(err => Native.c4db_beginTransaction(Db, err));
                LiteCoreBridge.Check(err => NativeRaw.c4raw_put(Db, C4Slice.Constant("test"), key, meta, 
                    Body, err));
                LiteCoreBridge.Check(err => Native.c4db_endTransaction(Db, true, err));

                var doc = (C4RawDocument *)LiteCoreBridge.Check(err => NativeRaw.c4raw_get(Db,
                    C4Slice.Constant("test"), key, err));
                doc->key.Equals(key).Should().BeTrue("because the key should not change");
                doc->meta.Equals(meta).Should().BeTrue("because the meta should not change");
                doc->body.Equals(Body).Should().BeTrue("because the body should not change");
                Native.c4raw_free(doc);

                // Nonexistent:
                C4Error error;
                ((long)Native.c4raw_get(Db, "test", "bogus", &error)).Should().Be(0, 
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
                C4Document* doc = NativeRaw.c4doc_get(Db, DocID, true, &error);
                ((long)doc).Should().Be(0, "because the document does not exist");
                error.Domain.Should().Be(C4ErrorDomain.LiteCore);
                error.Code.Should().Be((int)LiteCoreError.NotFound);
                Native.c4doc_free(doc);

                // Now get the doc with mustExist=false, which returns an empty doc:
                doc = (C4Document *)LiteCoreBridge.Check(err => NativeRaw.c4doc_get(Db, DocID, false, err));
                ((int)doc->flags).Should().Be(0, "because the document is empty");
                doc->docID.Equals(DocID).Should().BeTrue("because the doc ID should match what was stored");
                ((long)doc->revID.buf).Should().Be(0, "because the doc has no revision ID yet");
                ((long)doc->selectedRev.revID.buf).Should().Be(0, "because the doc has no revision ID yet");
                Native.c4doc_free(doc);

                LiteCoreBridge.Check(err => Native.c4db_beginTransaction(Db, err));
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
                            return Native.c4doc_put(Db, &localRq, null, err);
                        });
                        doc->revID.Equals(RevID).Should().BeTrue("because the doc should have the stored revID");
                        doc->selectedRev.revID.Equals(RevID).Should().BeTrue("because the doc should have the stored revID");
                        doc->selectedRev.flags.Should().Be(C4RevisionFlags.Leaf, "because this is a leaf revision");
                        doc->selectedRev.body.Equals(Body).Should().BeTrue("because the body should be stored correctly");
                        Native.c4doc_free(doc);
                    }
                } finally {
                    LiteCoreBridge.Check(err => Native.c4db_endTransaction(Db, true, err));
                }

                // Reload the doc:
                doc = (C4Document *)LiteCoreBridge.Check(err => NativeRaw.c4doc_get(Db, DocID, true, err));
                doc->flags.Should().Be(C4DocumentFlags.Exists, "because this is an existing document");
                doc->docID.Equals(DocID).Should().BeTrue("because the doc should have the stored doc ID");
                doc->revID.Equals(RevID).Should().BeTrue("because the doc should have the stored rev ID");
                doc->selectedRev.revID.Equals(RevID).Should().BeTrue("because the doc should have the stored rev ID");
                doc->selectedRev.sequence.Should().Be(1, "because it is the first stored document");
                doc->selectedRev.body.Equals(Body).Should().BeTrue("because the doc should have the stored body");
                Native.c4doc_free(doc);

                // Get the doc by its sequence
                doc = (C4Document *)LiteCoreBridge.Check(err => Native.c4doc_getBySequence(Db, 1, err));
                doc->flags.Should().Be(C4DocumentFlags.Exists, "because this is an existing document");
                doc->docID.Equals(DocID).Should().BeTrue("because the doc should have the stored doc ID");
                doc->revID.Equals(RevID).Should().BeTrue("because the doc should have the stored rev ID");
                doc->selectedRev.revID.Equals(RevID).Should().BeTrue("because the doc should have the stored rev ID");
                doc->selectedRev.sequence.Should().Be(1, "because it is the first stored document");
                doc->selectedRev.body.Equals(Body).Should().BeTrue("because the doc should have the stored body");
                Native.c4doc_free(doc);
            });
        }

        [Fact]
        public void TestCreateMultipleRevisions()
        {
            RunTestVariants(() => {
                var docID = DocID.CreateString();
                var Body2 = C4Slice.Constant("{\"ok\":\"go\"}");
                CreateRev(docID, RevID, Body);
                CreateRev(docID, Rev2ID, Body2);
                CreateRev(docID, Rev2ID, Body2, false); // test redundant insert

                // Reload the doc:
                var doc = (C4Document *)LiteCoreBridge.Check(err => Native.c4doc_get(Db, docID, true, err));
                doc->flags.Should().Be(C4DocumentFlags.Exists);
                doc->docID.Equals(DocID).Should().BeTrue("because the doc should have the stored doc ID");
                doc->revID.Equals(Rev2ID).Should().BeTrue("because the doc should have the current rev ID");
                doc->selectedRev.revID.Equals(Rev2ID).Should().BeTrue("because the current revision is selected");
                doc->selectedRev.sequence.Should().Be(2L, "because the current revision is the second one");
                doc->selectedRev.body.Equals(Body2).Should().BeTrue("because the selected rev should have the correct body");

                if(Versioning == C4DocumentVersioning.RevisionTrees) {
                    // Select 1st revision:
                    Native.c4doc_selectParentRevision(doc).Should().BeTrue("because otherwise selecting the parent revision failed");
                    doc->selectedRev.revID.Equals(RevID).Should().BeTrue("because the first revision is selected");
                    doc->selectedRev.sequence.Should().Be(1L, "because the first revision is selected");
                    doc->selectedRev.body.Equals(C4Slice.Null).Should().BeTrue("because the body hasn't been loaded");
                    Native.c4doc_hasRevisionBody(doc).Should().BeTrue("because the body still exists");
                    LiteCoreBridge.Check(err => Native.c4doc_loadRevisionBody(doc, err));
                    doc->selectedRev.body.Equals(Body).Should().BeTrue("because the body is loaded");
                    Native.c4doc_selectParentRevision(doc).Should().BeFalse("because the root revision is selected");
                    Native.c4doc_free(doc);

                    // Compact database:
                    LiteCoreBridge.Check(err => Native.c4db_compact(Db, err));

                    doc = (C4Document *)LiteCoreBridge.Check(err => NativeRaw.c4doc_get(Db, DocID, true, err));
                    Native.c4doc_selectParentRevision(doc).Should().BeTrue("because otherwise selecting the parent revision failed");
                    doc->selectedRev.revID.Equals(RevID).Should().BeTrue("because the first revision is selected");
                    doc->selectedRev.sequence.Should().Be(1L, "because the first revision is selected");
                    if(Storage != C4StorageEngine.SQLite) {
                        doc->selectedRev.body.Equals(C4Slice.Null).Should().BeTrue("because the body is not available");
                        Native.c4doc_hasRevisionBody(doc).Should().BeFalse("because the body has been compacted");
                        C4Error error;
                        Native.c4doc_loadRevisionBody(doc, &error).Should().BeFalse("because the body is unavailable");
                    }

                    LiteCoreBridge.Check(err => Native.c4db_beginTransaction(Db, err));
                    try {
                        C4Error error;
                        NativeRaw.c4doc_purgeRevision(doc, Rev2ID, &error).Should().Be(2);
                        LiteCoreBridge.Check(err => Native.c4doc_save(doc, 20, err));
                    } finally {
                        LiteCoreBridge.Check(err => Native.c4db_endTransaction(Db, true, err));
                    }

                    Native.c4doc_free(doc);
                }
            });
        }

        [Fact]
        public void TestGetForPut()
        {
            RunTestVariants(() => {
                LiteCoreBridge.Check(err => Native.c4db_beginTransaction(Db, err));
                try {
                    // Creating doc given ID:
                    var doc = (C4Document *)LiteCoreBridge.Check(err => NativeRaw.c4doc_getForPut(Db,
                        DocID, C4Slice.Null, false, false, err));
                    doc->docID.Equals(DocID).Should().BeTrue("because the doc should have the correct doc ID");
                    doc->revID.Equals(C4Slice.Null).Should().BeTrue("because a rev ID has not been assigned yet");
                    ((int)doc->flags).Should().Be(0, "because the document has no flags yet");
                    doc->selectedRev.revID.Equals(C4Slice.Null).Should().BeTrue("because no rev ID has been assigned yet");
                    Native.c4doc_free(doc);

                    // Creating doc, no ID:
                    doc = (C4Document *)LiteCoreBridge.Check(err => NativeRaw.c4doc_getForPut(Db,
                        C4Slice.Null, C4Slice.Null, false, false, err));
                    doc->docID.size.Should().BeGreaterOrEqualTo(20, "because the document should be assigned a random ID");
                    doc->revID.Equals(C4Slice.Null).Should().BeTrue("because the doc doesn't have a rev ID yet");
                    ((int)doc->flags).Should().Be(0, "because the document has no flags yet");
                    doc->selectedRev.revID.Equals(C4Slice.Null).Should().BeTrue("because no rev ID has been assigned yet");
                    Native.c4doc_free(doc);

                    // Delete with no revID given
                    C4Error error;
                    doc = NativeRaw.c4doc_getForPut(Db, C4Slice.Null, C4Slice.Null, true, false, &error);
                    ((long)doc).Should().Be(0, "because the document does not exist");
                    error.Code.Should().Be((int)LiteCoreError.NotFound, "because the correct error should be returned");

                    // Adding new rev of nonexistent doc:
                    doc = NativeRaw.c4doc_getForPut(Db, DocID, RevID, false, false, &error);
                    ((long)doc).Should().Be(0, "because the document does not exist");
                    error.Code.Should().Be((int)LiteCoreError.NotFound, "because the correct error should be returned");

                    // Adding new rev of existing doc:
                    CreateRev(DocID.CreateString(), RevID, Body);
                    doc = (C4Document *)LiteCoreBridge.Check(err => NativeRaw.c4doc_getForPut(Db, DocID, RevID, false,
                        false, err));
                    doc->docID.Equals(DocID).Should().BeTrue("because the doc should have the correct doc ID");
                    doc->revID.Equals(RevID).Should().BeTrue("because the doc should have the correct rev ID");
                    doc->flags.Should().Be(C4DocumentFlags.Exists, "because the document has no flags yet");
                    doc->selectedRev.revID.Equals(RevID).Should().BeTrue("because the selected rev should have the correct rev ID");
                    Native.c4doc_free(doc);

                    // Adding new rev, with nonexistent parent
                    doc = NativeRaw.c4doc_getForPut(Db, DocID, Rev2ID, false, false, &error);
                    ((long)doc).Should().Be(0, "because the document does not exist");
                    error.Code.Should().Be((int)LiteCoreError.Conflict, "because the correct error should be returned");

                    // Conflict -- try & fail to update non-current rev:
                    var body2 = C4Slice.Constant("{\"ok\":\"go\"}");
                    CreateRev(DocID.CreateString(), Rev2ID, body2);
                    doc = NativeRaw.c4doc_getForPut(Db, DocID, RevID, false, false, &error);
                    ((long)doc).Should().Be(0, "because the document does not exist");
                    error.Code.Should().Be((int)LiteCoreError.Conflict, "because the correct error should be returned");

                    if(Versioning == C4DocumentVersioning.RevisionTrees) {
                        // Conflict -- force an update of non-current rev:
                        doc = (C4Document *)LiteCoreBridge.Check(err => NativeRaw.c4doc_getForPut(Db, DocID, 
                            RevID, false, true, err));
                        doc->docID.Equals(DocID).Should().BeTrue("because the doc should have the correct doc ID");
                        doc->selectedRev.revID.Equals(RevID).Should().BeTrue("because the doc should have the correct rev ID");
                        Native.c4doc_free(doc);
                    }

                    // Deleting the doc:
                    doc = (C4Document *)LiteCoreBridge.Check(err => NativeRaw.c4doc_getForPut(Db, DocID, 
                        Rev2ID, true, false, err));
                    doc->docID.Equals(DocID).Should().BeTrue("because the doc should have the correct doc ID");
                    doc->selectedRev.revID.Equals(Rev2ID).Should().BeTrue("because the doc should have the correct rev ID");
                    Native.c4doc_free(doc);

                    // Actually tdelete it:
                    CreateRev(DocID.CreateString(), Rev3ID, C4Slice.Null);

                    // Re-creating the doc (no revID given):
                    doc = (C4Document *)LiteCoreBridge.Check(err => NativeRaw.c4doc_getForPut(Db, DocID, 
                        C4Slice.Null, false, false, err));
                    doc->docID.Equals(DocID).Should().BeTrue("because the doc should have the correct doc ID");
                    doc->selectedRev.revID.Equals(Rev3ID).Should().BeTrue("because the doc should have the correct rev ID");
                    doc->flags.Should().Be(C4DocumentFlags.Exists|C4DocumentFlags.Deleted, "because the document was deleted");
                    doc->selectedRev.revID.Equals(Rev3ID).Should().BeTrue("because the doc should have the correct rev ID");
                    Native.c4doc_free(doc);
                } finally {
                    LiteCoreBridge.Check(err => Native.c4db_endTransaction(Db, true, err));
                }
            });
        }

        [Fact]
        public void TestPut()
        {
            RunTestVariants(() => {
            LiteCoreBridge.Check(err => Native.c4db_beginTransaction(Db, err));
                try {
                    // Creating doc given ID:
                    var rq = new C4DocPutRequest {
                        docID = DocID,
                        body = Body,
                        save = true
                    };

                    var doc = (C4Document *)LiteCoreBridge.Check(err => {
                        var localRq = rq;
                        return Native.c4doc_put(Db, &localRq, null, err);
                    });

                    doc->docID.Equals(DocID).Should().BeTrue("because the doc should have the correct doc ID");
                    var expectedRevID = IsRevTrees() ? C4Slice.Constant("1-c10c25442d9fe14fa3ca0db4322d7f1e43140fab") :
                        C4Slice.Constant("1@*");
                    doc->revID.Equals(expectedRevID).Should().BeTrue("because the doc should have the correct rev ID");
                    doc->flags.Should().Be(C4DocumentFlags.Exists, "because the document exists");
                    doc->selectedRev.revID.Equals(expectedRevID).Should().BeTrue("because the selected rev should have the correct rev ID");
                    Native.c4doc_free(doc);

                    // Update doc:
                    var tmp = new[] { expectedRevID };
                    rq.body = C4Slice.Constant("{\"ok\":\"go\"}");
                    rq.historyCount = 1;
                    ulong commonAncestorIndex = 0UL;
                    fixed(C4Slice* history = tmp) {
                        rq.history = history;
                        doc = (C4Document *)LiteCoreBridge.Check(err => {
                            var localRq = rq;
                            ulong cai;
                            var retVal = Native.c4doc_put(Db, &localRq, &cai, err);
                            commonAncestorIndex = cai;
                            return retVal;
                        });
                    }

                    commonAncestorIndex.Should().Be(1UL, "because the common ancestor is at sequence 1");
                    var expectedRev2ID = IsRevTrees() ? C4Slice.Constant("2-32c711b29ea3297e27f3c28c8b066a68e1bb3f7b") :
                        C4Slice.Constant("2@*");
                    doc->revID.Equals(expectedRev2ID).Should().BeTrue("because the doc should have the updated rev ID");
                    doc->flags.Should().Be(C4DocumentFlags.Exists, "because the document exists");
                    doc->selectedRev.revID.Equals(expectedRev2ID).Should().BeTrue("because the selected rev should have the correct rev ID");
                    Native.c4doc_free(doc);

                    // Insert existing rev that conflicts:
                    rq.body = C4Slice.Constant("{\"from\":\"elsewhere\"}");
                    rq.existingRevision = true;
                    var conflictRevID = IsRevTrees() ? C4Slice.Constant("2-deadbeef") : C4Slice.Constant("1@binky");
                    tmp = new[] { conflictRevID, expectedRevID };
                    rq.historyCount = 2;
                    fixed(C4Slice* history = tmp) {
                        rq.history = history;
                        doc = (C4Document *)LiteCoreBridge.Check(err => {
                            var localRq = rq;
                            ulong cai;
                            var retVal = Native.c4doc_put(Db, &localRq, &cai, err);
                            commonAncestorIndex = cai;
                            return retVal;
                        });
                    }

                    commonAncestorIndex.Should().Be(1UL, "because the common ancestor is at sequence 1");
                    doc->flags.Should().Be(C4DocumentFlags.Exists|C4DocumentFlags.Conflicted, "because the document exists");
                    doc->selectedRev.revID.Equals(conflictRevID).Should().BeTrue("because the selected rev should have the correct rev ID");
                    if(IsRevTrees()) {
                        doc->revID.Equals(conflictRevID).Should().BeTrue("because the doc should have the conflicted rev ID");
                    } else {
                        doc->revID.Equals(expectedRev2ID).Should().BeTrue("because the doc should have the winning rev ID");
                    }
                    Native.c4doc_free(doc);
                } finally {
                    LiteCoreBridge.Check(err => Native.c4db_endTransaction(Db, true, err));
                }
            });
        }

        [Fact]
        public void TestAllDocs()
        {
            RunTestVariants(() => {
                SetupAllDocs();

                Native.c4db_getDocumentCount(Db).Should().Be(99UL, "because there are 99 non-deleted documents");

                // No start or end ID:
                var options = C4EnumeratorOptions.Default;
                options.flags &= ~C4EnumeratorFlags.IncludeBodies;
                var e = (C4DocEnumerator *)LiteCoreBridge.Check(err => {
                    var localOpts = options;
                    return Native.c4db_enumerateAlLDocs(Db, null, null, &localOpts, err);
                });

                int i = 1;
                C4Error error;
                while(Native.c4enum_next(e, &error)) {
                    var doc = (C4Document *)LiteCoreBridge.Check(err => Native.c4enum_getDocument(e, err));
                    var docID = $"doc-{i:D3}";
                    doc->docID.CreateString().Should().Be(docID, "because the doc should have the correct doc ID");
                    doc->revID.Equals(RevID).Should().BeTrue("because the doc should have the current revID");
                    doc->selectedRev.revID.Equals(RevID).Should().BeTrue("because the selected rev should have the correct rev ID");
                    doc->selectedRev.sequence.Should().Be((ulong)i, "because the sequences should come in order");
                    doc->selectedRev.body.Equals(C4Slice.Null).Should().BeTrue("because the body is not loaded yet");
                    LiteCoreBridge.Check(err => Native.c4doc_loadRevisionBody(doc, err));
                    doc->selectedRev.body.Equals(Body).Should().BeTrue("because the loaded body should be correct");

                    C4DocumentInfo info;
                    Native.c4enum_getDocumentInfo(e, &info).Should().BeTrue("because otherwise the doc info load failed");
                    info.docID.CreateString().Should().Be(docID, "because the doc info should have the correct doc ID");
                    info.revID.Equals(RevID).Should().BeTrue("because the doc info should have the correct rev ID");

                    Native.c4doc_free(doc);
                    i++;
                }

                Native.c4enum_free(e);
                i.Should().Be(100);

                // Start and end ID:
                e = (C4DocEnumerator *)LiteCoreBridge.Check(err => Native.c4db_enumerateAlLDocs(Db, 
                    "doc-007", "doc-090", null, err));
                i = 7;
                while(Native.c4enum_next(e, &error)) {
                    error.Code.Should().Be(0, "because otherwise an enumeration error occurred");
                    var doc = (C4Document *)LiteCoreBridge.Check(err => Native.c4enum_getDocument(e, err));
                    var docID = $"doc-{i:D3}";
                    doc->docID.CreateString().Should().Be(docID, "because the doc should have the correct doc ID");
                    Native.c4doc_free(doc);
                    i++;
                }

                Native.c4enum_free(e);
                i.Should().Be(91, "because that is how many documents fall in the given range");

                // Some docs, by ID:
                options = C4EnumeratorOptions.Default;
                options.flags |= C4EnumeratorFlags.IncludeDeleted;
                var docIDs = new[] { "doc-042", "doc-007", "bogus", "doc-001" };
                e = (C4DocEnumerator *)LiteCoreBridge.Check(err => {
                    var localOpts = options;
                    return Native.c4db_enumerateSomeDocs(Db, docIDs, &localOpts, err);
                });

                i = 0;
                while(Native.c4enum_next(e, &error)) {
                    error.Code.Should().Be(0, "because otherwise an enumeration error occurred");
                    var doc = (C4Document *)LiteCoreBridge.Check(err => Native.c4enum_getDocument(e, err));
                    doc->docID.CreateString().Should().Be(docIDs[i], "because the doc should have the correct sorted doc ID");
                    if(doc->sequence != 0) {
                        i.Should().NotBe(2, "because no document exists with the 'bogus' key");
                    }

                    Native.c4doc_free(doc);
                    i++;
                }

                Native.c4enum_free(e);
                i.Should().Be(4, "because four document IDs were specified");
            });
        }

        [Fact]
        public void TestAllDocsIncludeDeleted()
        {
            RunTestVariants(() => {
                SetupAllDocs();
                var options = C4EnumeratorOptions.Default;
                options.flags |= C4EnumeratorFlags.IncludeDeleted;
                var e = (C4DocEnumerator *)LiteCoreBridge.Check(err => {
                    var localOpts = options;
                    return Native.c4db_enumerateAlLDocs(Db, "doc-004", "doc-007", &localOpts, err);
                });

                int i = 4;
                C4Error error;
                while(Native.c4enum_next(e, &error)) {
                    var doc = (C4Document *)LiteCoreBridge.Check(err => Native.c4enum_getDocument(e, err));
                    var docID = default(string);
                    if(i == 6) {
                        docID = "doc-005DEL";
                    } else {
                        var docNum = i >= 6 ? i - 1 : i;
                        docID = $"doc-{docNum:D3}";
                    }

                    doc->docID.CreateString().Should().Be(docID, "because the doc should have the correct doc ID");
                    Native.c4doc_free(doc);
                    i++;
                }

                Native.c4enum_free(e);
                i.Should().Be(9, "because that is the last ID suffix in the given range");
            });
        }

        [Fact]
        public void TestAllDocsInfo()
        {
            RunTestVariants(() => {
                SetupAllDocs();

                var options = C4EnumeratorOptions.Default;
                var e = (C4DocEnumerator *)LiteCoreBridge.Check(err => {
                    var localOpts = options;
                    return Native.c4db_enumerateAlLDocs(Db, null, null, &localOpts, err);
                });

                int i = 1;
                C4Error error;
                while(Native.c4enum_next(e, &error)) {
                    C4DocumentInfo doc;
                    Native.c4enum_getDocumentInfo(e, &doc).Should().BeTrue("because otherwise getting the doc info failed");
                    var docID = $"doc-{i:D3}";
                    doc.docID.CreateString().Should().Be(docID, "because the doc info should have the correct doc ID");
                    doc.revID.Equals(RevID).Should().BeTrue("because the doc info should have the correct rev ID");
                    doc.sequence.Should().Be((ulong)i, "because the doc info should have the correct sequence");
                    doc.flags.Should().Be(C4DocumentFlags.Exists, "because the doc info should have the correct flags");
                    i++;
                }

                Native.c4enum_free(e);
                error.Code.Should().Be(0, "because otherwise an error occurred somewhere");
                i.Should().Be(100, "because all docs should be iterated, even deleted ones");
            });
        }

        [Fact]
        public void TestChanges()
        {
            RunTestVariants(() => {
                for(int i = 1; i < 100; i++) {
                    var docID = $"doc-{i:D3}";
                    CreateRev(docID, RevID, Body);
                }

                // Since start:
                var options = C4EnumeratorOptions.Default;
                options.flags &= ~C4EnumeratorFlags.IncludeBodies;
                var e = (C4DocEnumerator *)LiteCoreBridge.Check(err => {
                    var localOpts = options;
                    return Native.c4db_enumerateChanges(Db, 0, &localOpts, err);
                });

                var seq = 1UL;
                C4Document* doc;
                C4Error error;
                while(null != (doc = Native.c4enum_nextDocument(e, &error))) {
                    doc->selectedRev.sequence.Should().Be(seq, "because the sequence numbers should be ascending");
                    var docID = $"doc-{seq:D3}";
                    doc->docID.CreateString().Should().Be(docID, "because the doc should have the correct doc ID");
                    Native.c4doc_free(doc);
                    seq++;
                }

                Native.c4enum_free(e);

                // Since 6:
                e = (C4DocEnumerator *)LiteCoreBridge.Check(err => {
                    var localOpts = options;
                    return Native.c4db_enumerateChanges(Db, 6, &localOpts, err);
                });

                seq = 7;
                while(null != (doc = Native.c4enum_nextDocument(e, &error))) {
                    doc->selectedRev.sequence.Should().Be(seq, "because the sequence numbers should be ascending");
                    var docID = $"doc-{seq:D3}";
                    doc->docID.CreateString().Should().Be(docID, "because the doc should have the correct doc ID");
                    Native.c4doc_free(doc);
                    seq++;
                }

                Native.c4enum_free(e);
                seq.Should().Be(100UL, "because that is the highest sequence in the DB");
            });
        }

        [Fact]
        public void TestExpired()
        {
            RunTestVariants(() => {
                const string docID = "expire_me";
                CreateRev(docID, RevID, Body);
                var expire = DateTimeOffset.UtcNow.Add(TimeSpan.FromSeconds(1)).ToUnixTimeSeconds();
                LiteCoreBridge.Check(err => Native.c4doc_setExpiration(Db, docID, (ulong)expire, err));

                expire = DateTimeOffset.UtcNow.Add(TimeSpan.FromSeconds(2)).ToUnixTimeSeconds();
                // Make sure setting it to the same is also true
                LiteCoreBridge.Check(err => Native.c4doc_setExpiration(Db, docID, (ulong)expire, err));
                LiteCoreBridge.Check(err => Native.c4doc_setExpiration(Db, docID, (ulong)expire, err));

                const string docID2 = "expire_me_too";
                CreateRev(docID2, RevID, Body);
                LiteCoreBridge.Check(err => Native.c4doc_setExpiration(Db, docID2, (ulong)expire, err));

                const string docID3 = "dont_expire_me";
                CreateRev(docID3, RevID, Body);
                Task.Delay(TimeSpan.FromSeconds(2)).Wait();

                var e = (C4ExpiryEnumerator *)LiteCoreBridge.Check(err => Native.c4db_enumerateExpired(Db, err));
                int expiredCount = 0;
                while(Native.c4exp_next(e, null)) {
                    var existingDocID = Native.c4exp_getDocID(e);
                    existingDocID.Should().NotBe(docID3, "because the last document is not scheduled for expiration");
                    expiredCount++;
                }

                Native.c4exp_free(e);
                expiredCount.Should().Be(2, "because 2 documents were scheduled for expiration");
                Native.c4doc_getExpiration(Db, docID).Should().Be((ulong)expire, "because that was what was set as the expiration");
                Native.c4doc_getExpiration(Db, docID2).Should().Be((ulong)expire, "because that was what was set as the expiration");
                Native.c4db_nextDocExpiration(Db).Should().Be((ulong)expire, "because that is the closest expiration date");

                e = (C4ExpiryEnumerator *)LiteCoreBridge.Check(err => Native.c4db_enumerateExpired(Db, err));
                expiredCount = 0;
                while(Native.c4exp_next(e, null)) {
                    var existingDocID = Native.c4exp_getDocID(e);
                    existingDocID.Should().NotBe(docID3, "because the last document is not scheduled for expiration");
                    expiredCount++;
                }

                LiteCoreBridge.Check(err => Native.c4exp_purgeExpired(e, err));
                Native.c4exp_free(e);
                expiredCount.Should().Be(2, "because 2 documents were scheduled for expiration");
                
                e = (C4ExpiryEnumerator *)LiteCoreBridge.Check(err => Native.c4db_enumerateExpired(Db, err));
                expiredCount = 0;
                while(Native.c4exp_next(e, null)) {
                    expiredCount++;
                }

                LiteCoreBridge.Check(err => Native.c4exp_purgeExpired(e, err));
                Native.c4exp_free(e);
                expiredCount.Should().Be(0, "because no more documents were scheduled for expiration");
            });
        }

        [Fact]
        public void TestCancelExpire()
        {
            RunTestVariants(() => {
                const string docID = "expire_me";
                CreateRev(docID, RevID, Body);
                var expire = (ulong)DateTimeOffset.UtcNow.AddSeconds(2).ToUnixTimeSeconds();
                LiteCoreBridge.Check(err => Native.c4doc_setExpiration(Db, docID, expire, err));
                LiteCoreBridge.Check(err => Native.c4doc_setExpiration(Db, docID, UInt64.MaxValue, err));

                Task.Delay(TimeSpan.FromSeconds(2)).Wait();
                var e = (C4ExpiryEnumerator *)LiteCoreBridge.Check(err => Native.c4db_enumerateExpired(Db, err));

                int expiredCount = 0;
                while(Native.c4exp_next(e, null)) {
                    expiredCount++;
                }

                LiteCoreBridge.Check(err => Native.c4exp_purgeExpired(e, err));
                Native.c4exp_free(e);
                expiredCount.Should().Be(0, "because the expiration was cancelled");    
            });
        }

        private void SetupAllDocs()
        {
            for(int i = 1; i < 100; i++) {
                var docID = $"doc-{i:D3}";
                CreateRev(docID, RevID, Body);
            }

            // Add a deleted doc to make sure it's skipped by default:
            CreateRev("doc-005DEL", RevID, C4Slice.Null);
        }

        private void AssertMessage(C4ErrorDomain domain, int code, string expected)
        {
            var msg = Native.c4error_getMessage(new C4Error(domain, code));
            msg.Should().Be(expected, "because the error message should match the code");
        }
    }
}