using FluentAssertions;
using LiteCore.Interop;
using Xunit;

namespace LiteCore.Tests
{
    public unsafe class DocumentTest : Test
    {
        [Fact]
        public void TestFleeceDocs()
        {
            RunTestVariants(() => {
                ImportJSONLines("../../../C/tests/data/names_100.json");
            });
        }

        [Fact]
        public void TestGetForPut()
        {
            RunTestVariants(() => {
                LiteCoreBridge.Check(err => Native.c4db_beginTransaction(Db, err));
                try {
                    // Creating doc given ID:
                    var doc = (C4Document *)LiteCoreBridge.Check(err => NativeRawPrivate.c4doc_getForPut(Db,
                        DocID, C4Slice.Null, false, false, err));
                    doc->docID.Equals(DocID).Should().BeTrue("because the doc should have the correct doc ID");
                    doc->revID.Equals(C4Slice.Null).Should().BeTrue("because a rev ID has not been assigned yet");
                    ((int)doc->flags).Should().Be(0, "because the document has no flags yet");
                    doc->selectedRev.revID.Equals(C4Slice.Null).Should().BeTrue("because no rev ID has been assigned yet");
                    Native.c4doc_free(doc);

                    // Creating doc, no ID:
                    doc = (C4Document *)LiteCoreBridge.Check(err => NativeRawPrivate.c4doc_getForPut(Db,
                        C4Slice.Null, C4Slice.Null, false, false, err));
                    doc->docID.size.Should().BeGreaterOrEqualTo(20, "because the document should be assigned a random ID");
                    doc->revID.Equals(C4Slice.Null).Should().BeTrue("because the doc doesn't have a rev ID yet");
                    ((int)doc->flags).Should().Be(0, "because the document has no flags yet");
                    doc->selectedRev.revID.Equals(C4Slice.Null).Should().BeTrue("because no rev ID has been assigned yet");
                    Native.c4doc_free(doc);

                    // Delete with no revID given
                    C4Error error;
                    doc = NativeRawPrivate.c4doc_getForPut(Db, C4Slice.Null, C4Slice.Null, true, false, &error);
                    ((long)doc).Should().Be(0, "because the document does not exist");
                    error.code.Should().Be((int)LiteCoreError.NotFound, "because the correct error should be returned");

                    // Adding new rev of nonexistent doc:
                    doc = NativeRawPrivate.c4doc_getForPut(Db, DocID, RevID, false, false, &error);
                    ((long)doc).Should().Be(0, "because the document does not exist");
                    error.code.Should().Be((int)LiteCoreError.NotFound, "because the correct error should be returned");

                    // Adding new rev of existing doc:
                    CreateRev(DocID.CreateString(), RevID, Body);
                    doc = (C4Document *)LiteCoreBridge.Check(err => NativeRawPrivate.c4doc_getForPut(Db, DocID, RevID, false,
                        false, err));
                    doc->docID.Equals(DocID).Should().BeTrue("because the doc should have the correct doc ID");
                    doc->revID.Equals(RevID).Should().BeTrue("because the doc should have the correct rev ID");
                    doc->flags.Should().Be(C4DocumentFlags.Exists, "because the document has no flags yet");
                    doc->selectedRev.revID.Equals(RevID).Should().BeTrue("because the selected rev should have the correct rev ID");
                    Native.c4doc_free(doc);

                    // Adding new rev, with nonexistent parent
                    doc = NativeRawPrivate.c4doc_getForPut(Db, DocID, Rev2ID, false, false, &error);
                    ((long)doc).Should().Be(0, "because the document does not exist");
                    error.code.Should().Be((int)LiteCoreError.Conflict, "because the correct error should be returned");

                    // Conflict -- try & fail to update non-current rev:
                    var body2 = C4Slice.Constant("{\"ok\":\"go\"}");
                    CreateRev(DocID.CreateString(), Rev2ID, body2);
                    doc = NativeRawPrivate.c4doc_getForPut(Db, DocID, RevID, false, false, &error);
                    ((long)doc).Should().Be(0, "because the document does not exist");
                    error.code.Should().Be((int)LiteCoreError.Conflict, "because the correct error should be returned");

                    if(Versioning == C4DocumentVersioning.RevisionTrees) {
                        // Conflict -- force an update of non-current rev:
                        doc = (C4Document *)LiteCoreBridge.Check(err => NativeRawPrivate.c4doc_getForPut(Db, DocID, 
                            RevID, false, true, err));
                        doc->docID.Equals(DocID).Should().BeTrue("because the doc should have the correct doc ID");
                        doc->selectedRev.revID.Equals(RevID).Should().BeTrue("because the doc should have the correct rev ID");
                        Native.c4doc_free(doc);
                    }

                    // Deleting the doc:
                    doc = (C4Document *)LiteCoreBridge.Check(err => NativeRawPrivate.c4doc_getForPut(Db, DocID, 
                        Rev2ID, true, false, err));
                    doc->docID.Equals(DocID).Should().BeTrue("because the doc should have the correct doc ID");
                    doc->selectedRev.revID.Equals(Rev2ID).Should().BeTrue("because the doc should have the correct rev ID");
                    Native.c4doc_free(doc);

                    // Actually delete it:
                    CreateRev(DocID.CreateString(), Rev3ID, C4Slice.Null, C4RevisionFlags.Deleted);

                    LiteCoreBridge.Check(err => Native.c4db_endTransaction(Db, true, err));
                    LiteCoreBridge.Check(err => Native.c4db_beginTransaction(Db, err));

                    // Re-creating the doc (no revID given):
                    doc = (C4Document *)LiteCoreBridge.Check(err => NativeRawPrivate.c4doc_getForPut(Db, DocID, 
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
    }
}