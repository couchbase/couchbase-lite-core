//
//  C4DatabaseTest.cs
//
//  Author:
//  	Jim Borden  <jim.borden@couchbase.com>
//
//  Copyright (c) 2015 Couchbase, Inc All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License");
//  you may not use this file except in compliance with the License.
//  You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.
//
//#define FAKE_ENCRYPTION
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

using NUnit.Framework;

namespace CBForest.Tests
{
    internal static class TestNative
    {
        [DllImport("msvcrt.dll", CallingConvention=CallingConvention.Cdecl)]
        public static extern int unlink(string filename);
    }
    
    internal unsafe sealed class TransactionHelper : IDisposable
    {
        private readonly C4Database *_db;
        
        public TransactionHelper(C4Database *db)
        {
            C4Error error;
            Assert.IsTrue(Native.c4db_beginTransaction(db, &error));
            _db = db;
        }
        
        public void Dispose()
        {
            if(_db != null) {
                C4Error error;
                Assert.IsTrue(Native.c4db_endTransaction(_db, true, &error));
            }
        }
    }
    
    [TestFixture]
    public unsafe class C4DatabaseTest : C4Test
    {
        [Test]
        public void TestTransaction()
        {
            Assert.AreEqual(0, Native.c4db_getDocumentCount(_db));
            Assert.IsFalse(Native.c4db_isInTransaction(_db));
            C4Error error;
            Assert.IsTrue(Native.c4db_beginTransaction(_db, &error));
            Assert.IsTrue(Native.c4db_isInTransaction(_db));
            Assert.IsTrue(Native.c4db_beginTransaction(_db, &error));
            Assert.IsTrue(Native.c4db_isInTransaction(_db));
            Assert.IsTrue(Native.c4db_endTransaction(_db, true, &error));
            Assert.IsTrue(Native.c4db_isInTransaction(_db));
            Assert.IsTrue(Native.c4db_endTransaction(_db, true, &error));
            Assert.IsFalse(Native.c4db_isInTransaction(_db));
        }
        
        [Test]
        public void TestCreateRawDoc()
        {
            C4Error error;
            const string key = "key";
            const string meta = "meta";
            Assert.IsTrue(Native.c4db_beginTransaction(_db, &error));
            Native.c4raw_put(_db, "test", key, meta, BODY, &error);
            Assert.IsTrue(Native.c4db_endTransaction(_db, true, &error));
            
            var doc = Native.c4raw_get(_db, "test", key, &error);
            Assert.IsTrue(doc != null);
            Assert.IsTrue(doc->key.Equals(key));
            Assert.IsTrue(doc->meta.Equals(meta));
            Assert.IsTrue(doc->body.Equals(BODY));
            Native.c4raw_free(doc);
        }
        
        [Test]
        public void TestCreateVersionedDoc()
        {
            // Try reading doc with mustExist=true, which should fail:
            C4Error error;
            var doc = Native.c4doc_get(_db, DOC_ID, true, &error);
            Assert.IsTrue(doc == null);
            Assert.AreEqual(C4ErrorDomain.ForestDB, error.domain);
            Assert.AreEqual(ForestDBStatus.KeyNotFound, (ForestDBStatus)error.code);
            
            // Now get the doc with mustExist=false, which returns an empty doc:
            doc = Native.c4doc_get(_db, DOC_ID, false, &error);
            Assert.IsTrue(doc != null);
            Assert.AreEqual((C4DocumentFlags)0, doc->flags);
            Assert.IsTrue(doc->docID.Equals(DOC_ID));
            Assert.IsTrue(doc->revID.buf == null);
            Assert.IsTrue(doc->selectedRev.revID.buf == null);
            
            using(var t = new TransactionHelper(_db)) {
                Assert.AreEqual(1, Native.c4doc_insertRevision(doc, REV_ID, BODY, false, false, false, &error));
                Assert.IsTrue(doc->revID.Equals(REV_ID));
                Assert.IsTrue(doc->selectedRev.revID.Equals(REV_ID));
                Assert.AreEqual(C4RevisionFlags.RevNew | C4RevisionFlags.RevLeaf, doc->selectedRev.flags);
                Assert.IsTrue(doc->selectedRev.body.Equals(BODY));
                Assert.IsTrue(Native.c4doc_save(doc, 20, &error));
                Native.c4doc_free(doc);
            }
            
            // Reload the doc:
            doc = Native.c4doc_get(_db, DOC_ID, true, &error);
            Assert.IsTrue(doc != null);
            Assert.AreEqual(C4DocumentFlags.Exists, doc->flags);
            Assert.IsTrue(doc->docID.Equals(DOC_ID));
            Assert.IsTrue(doc->revID.Equals(REV_ID));
            Assert.IsTrue(doc->selectedRev.revID.Equals(REV_ID));
            Assert.AreEqual(1UL, doc->selectedRev.sequence);
            Assert.IsTrue(doc->selectedRev.body.Equals(BODY));
            
            // Get the doc by its sequence:
            doc = Native.c4doc_getBySequence(_db, 1, &error);
            Assert.IsTrue(doc != null);
            Assert.AreEqual(C4DocumentFlags.Exists, doc->flags);
            Assert.IsTrue(doc->docID.Equals(DOC_ID));
            Assert.IsTrue(doc->revID.Equals(REV_ID));
            Assert.IsTrue(doc->selectedRev.revID.Equals(REV_ID));
            Assert.AreEqual(1UL, doc->selectedRev.sequence);
            Assert.IsTrue(doc->selectedRev.body.Equals(BODY));
        }
        
        [Test]
        public void TestCreateMultipleRevisions()
        {
            const string BODY_2 = "{\"ok\":\"go\"}";
            CreateRev(DOC_ID, REV_ID, BODY);
            CreateRev(DOC_ID, REV_2_ID, BODY_2);
            CreateRev(DOC_ID, REV_2_ID, BODY_2, false); // test redundant insert
            
            // Reload the doc:
            C4Error error;
            var doc = Native.c4doc_get(_db, DOC_ID, true, &error);
            Assert.IsTrue(doc != null);
            Assert.AreEqual(C4DocumentFlags.Exists, doc->flags);
            Assert.IsTrue(doc->docID.Equals(DOC_ID));
            Assert.IsTrue(doc->revID.Equals(REV_2_ID));
            Assert.IsTrue(doc->selectedRev.revID.Equals(REV_2_ID));
            Assert.AreEqual(2UL, doc->selectedRev.sequence);
            Assert.IsTrue(doc->selectedRev.body.Equals(BODY_2));
            
            // Select 1st revision:
            Assert.IsTrue(Native.c4doc_selectParentRevision(doc));
            Assert.IsTrue(doc->selectedRev.revID.Equals(REV_ID));
            Assert.AreEqual(1UL, doc->selectedRev.sequence);
            Assert.AreEqual(C4Slice.NULL, doc->selectedRev.body);
            Assert.IsTrue(Native.c4doc_hasRevisionBody(doc));
            Assert.IsTrue(Native.c4doc_loadRevisionBody(doc, &error)); // have to explicitly load the body
            Assert.IsTrue(doc->selectedRev.body.Equals(BODY));
            Assert.IsFalse(Native.c4doc_selectParentRevision(doc));
            
            // Compact database:
            Assert.IsTrue(Native.c4db_compact(_db, &error));
            
            doc = Native.c4doc_get(_db, DOC_ID, true, &error);
            Assert.IsTrue(doc != null);
            Assert.IsTrue(Native.c4doc_selectParentRevision(doc));
            Assert.IsTrue(doc->selectedRev.revID.Equals(REV_ID));
            Assert.AreEqual(1L, doc->selectedRev.sequence);
            Assert.IsTrue(doc->selectedRev.body.Equals(C4Slice.NULL));
            Assert.IsFalse(Native.c4doc_hasRevisionBody(doc));
            Assert.IsFalse(Native.c4doc_loadRevisionBody(doc, &error));
            
            using(var t = new TransactionHelper(_db)) {
                int nPurged = Native.c4doc_purgeRevision(doc, REV_2_ID, &error);
                Assert.AreEqual(2, nPurged);
                Assert.IsTrue(Native.c4doc_save(doc, 20, &error));   
            }
        }
        
        [Test]
        public void TestAllDocs()
        {
            SetupAllDocs();
            C4Error error;
            C4Document* doc;
            
            var options = C4EnumeratorOptions.DEFAULT;
            options.flags &= ~C4EnumeratorFlags.IncludeBodies;
            var e = Native.c4db_enumerateAllDocs(_db, null, null, &options, &error);
            Assert.IsTrue(e != null);
            var j = 1UL;
            while(null != (doc = Native.c4enum_nextDocument(e, &error))) {
                var docID = String.Format("doc-{0}", j.ToString("D3"));
                Assert.IsTrue(doc->docID.Equals(docID));
                Assert.IsTrue(doc->revID.Equals(REV_ID));
                Assert.IsTrue(doc->selectedRev.revID.Equals(REV_ID));
                Assert.AreEqual(j, doc->selectedRev.sequence);
                Assert.AreEqual(C4Slice.NULL, doc->selectedRev.body);
                
                // Doc was loaded without its body, but it should load on demand:
                Assert.IsTrue(Native.c4doc_loadRevisionBody(doc, &error));
                Assert.IsTrue(doc->selectedRev.body.Equals(BODY));
                Native.c4doc_free(doc);
                j++;
            }
            
            Native.c4enum_free(e);
            
            // Start and end ID:
            e = Native.c4db_enumerateAllDocs(_db, "doc-007", "doc-090", null, &error);
            Assert.IsTrue(e != null);
            j = 7;
            while(null != (doc = Native.c4enum_nextDocument(e, &error))) {
                var docID = String.Format("doc-{0,3}", j.ToString("D3"));
                Assert.IsTrue(doc->docID.Equals(docID));
                Native.c4doc_free(doc);
                j++;
            }
            
            Assert.AreEqual(91, j);
            
            // Some docs, by ID:
            options = C4EnumeratorOptions.DEFAULT;
            options.flags |= C4EnumeratorFlags.IncludeDeleted;
            var docIDs = new[] { "doc-042", "doc-007", "bogus", "doc-001" };
            e = Native.c4db_enumerateSomeDocs(_db, docIDs, &options, &error);
            Assert.IsTrue(e != null);
            j = 0;
            while (null != (doc = Native.c4enum_nextDocument(e, &error))) {
                Assert.IsTrue(doc->docID.Equals(docIDs[j]));
                Assert.AreEqual(j != 2, doc->sequence != 0);
                Native.c4doc_free(doc);
                j++;
            }
            Assert.AreEqual(4, j);
            
        }
        
        [Test]
        public void TestChanges()
        {
            for(int i = 1; i < 100; i++) {
                var docID = String.Format("doc-{0,3}", i.ToString("D3"));
                CreateRev(docID, REV_ID, BODY);
            }

            C4Error error;
            C4Document* doc;

            // Since start:
            var options = C4EnumeratorOptions.DEFAULT;
            options.flags &= ~C4EnumeratorFlags.IncludeBodies;
            var e = Native.c4db_enumerateChanges(_db, 0, &options, &error);
            Assert.IsTrue(e != null);
            var seq = 1UL;
            while(null != (doc = Native.c4enum_nextDocument(e, &error))) {
                Assert.AreEqual(seq, doc->selectedRev.sequence);
                var docID = String.Format("doc-{0,3}", seq.ToString("D3"));
                Assert.IsTrue(doc->docID.Equals(docID));
                Native.c4doc_free(doc);
                seq++;
            }
            
            Native.c4enum_free(e);
            
            // Since 6:
            e = Native.c4db_enumerateChanges(_db, 6, &options, &error);
            Assert.IsTrue(e != null);
            seq = 7UL;
            while(null != (doc = Native.c4enum_nextDocument(e, &error))) {
                Assert.AreEqual(seq, doc->selectedRev.sequence);
                var docID = String.Format("doc-{0,3}", seq.ToString("D3"));
                Assert.IsTrue(doc->docID.Equals(docID));
                Native.c4doc_free(doc);
                seq++;
            } 
            
            Assert.AreEqual(100, seq);
        }
        
        [Test]
        public void TestInsertRevisionWithHistory()
        {
            const string BODY_2 = @"{""ok"":""go""}";
            CreateRev(DOC_ID, REV_ID, BODY);
            CreateRev(DOC_ID, REV_2_ID, BODY_2);
            
            // Reload the doc:
            C4Error error;
            var doc = Native.c4doc_get(_db, DOC_ID, true, &error);
            
            // Add 18 revisions; the last two entries in the history repeat the two existing revs:
            const int historyCount = 20;
            var revIDs = new List<string>(historyCount);
            var rand = new Random();
            for(uint i = historyCount - 1; i >= 2; i--) {
                revIDs.Add(String.Format("{0}-{1:x8}", i+1, rand.Next())); 
            }
            
            revIDs.Add(REV_2_ID);
            revIDs.Add(REV_ID);
            
            int n;
            using(var t = new TransactionHelper(_db)) {
                n = Native.c4doc_insertRevisionWithHistory(doc, @"{""foo"":true}", false, false, revIDs.ToArray(), &error);
            }
            
            if(n < 0) {
                Console.Error.WriteLine(String.Format("Error({0})", error));
            }
            
            Assert.AreEqual(historyCount - 2, n);
        }
        
        [Test]
        public void TestAllDocsIncludeDeleted()
        {
            SetupAllDocs();
            var error = default(C4Error);
            var e = default(C4DocEnumerator*);
            var doc = default(C4Document*);
            
            var options = C4EnumeratorOptions.DEFAULT;
            options.flags |= C4EnumeratorFlags.IncludeDeleted;
            e = Native.c4db_enumerateAllDocs(_db, "doc-004", "doc-007", &options, &error);
            Assert.IsTrue(e != null);
            int i = 4;
            while(null != (doc = Native.c4enum_nextDocument(e, &error))) {
                var offset = i > 6 ? 1 : 0;
                var docID = i == 6 ? "doc-005DEL" : String.Format("doc-{0,3}", (i - offset).ToString("D3"));
                Assert.IsTrue(doc->docID.Equals(docID));
                Native.c4doc_free(doc);
                i++;
            }
            
            Assert.AreEqual(9, i);
        }
        
        [Test]
        public void TestPut()
        {
            C4Error error;
            using(var t = new TransactionHelper(_db)) {
                C4DocPutRequest rq = new C4DocPutRequest();
                rq.docID = DOC_ID;
                rq.body = BODY;
                rq.save = true;
                var doc = Native.c4doc_put(_db, rq, null, &error);
                Assert.IsTrue(doc != null);
                Assert.IsTrue(doc->docID.Equals(DOC_ID));
                const string expectedRevID = "1-c10c25442d9fe14fa3ca0db4322d7f1e43140fab";
                Assert.IsTrue(doc->revID.Equals(expectedRevID));
                Assert.AreEqual(C4DocumentFlags.Exists, doc->flags);
                Assert.IsTrue(doc->selectedRev.revID.Equals(expectedRevID));
                Native.c4doc_free(doc);
                
                // Update doc:
                rq.body = "{\"ok\":\"go\"}";
                rq.history = new[] { expectedRevID };
                ulong commonAncestorIndex;
                doc = Native.c4doc_put(_db, rq, &commonAncestorIndex, &error);
                Assert.IsTrue(doc != null);
                Assert.AreEqual(1UL, commonAncestorIndex);
                const string expectedRev2ID = "2-32c711b29ea3297e27f3c28c8b066a68e1bb3f7b";
                Assert.IsTrue(doc->revID.Equals(expectedRev2ID));
                Assert.AreEqual(C4DocumentFlags.Exists, doc->flags);
                Assert.IsTrue(doc->selectedRev.revID.Equals(expectedRev2ID));
                Native.c4doc_free(doc);
                
                // Insert existing rev:
                rq.body = "{\"from\":\"elsewhere\"}";
                rq.existingRevision = true;
                rq.history = new[] { REV_2_ID, expectedRevID };
                doc = Native.c4doc_put(_db, rq, &commonAncestorIndex, &error);
                Assert.IsTrue(doc != null);
                Assert.AreEqual(1UL, commonAncestorIndex);
                Assert.IsTrue(doc->revID.Equals(REV_2_ID));
                Assert.AreEqual(C4DocumentFlags.Exists | C4DocumentFlags.Conflicted, doc->flags);
                Assert.IsTrue(doc->selectedRev.revID.Equals(REV_2_ID));
                Native.c4doc_free(doc);
            }
        }
        
        private void SetupAllDocs()
        {
            for(int i = 1; i < 100; i++) {
                var docID = String.Format("doc-{0,3}", i.ToString("D3"));
                CreateRev(docID, REV_ID, BODY);
            }
            
            // Add a deleted doc to make sure it's skipped by default:
            CreateRev("doc-005DEL", REV_ID, null);
        }
    }
    
    public unsafe class C4EncryptedDatabaseTest : C4DatabaseTest
    {
        private static C4EncryptionKey _EncryptionKey;
        
        protected override C4EncryptionKey* EncryptionKey
        {
            get { 
                fixed(C4EncryptionKey *p = &_EncryptionKey) {
                    return p;
                }
            }
        }  
        
        static C4EncryptedDatabaseTest()
        {
#if FAKE_ENCRYPTION
            _EncryptionKey.algorithm = (C4EncryptionAlgorithm)(-1);
#else
            _EncryptionKey.algorithm = C4EncryptionAlgorithm.AES256;
#endif
            var bytes = Encoding.UTF8.GetBytes("this is not a random key at all.");
            fixed(byte* src = bytes)
            fixed(byte* dst = _EncryptionKey.bytes) {
                Native.memcpy(dst, src, (UIntPtr)(uint)bytes.Length);
            }
        }
        
        [Test]
        public void TestRekey() 
        {
            TestCreateRawDoc();

            C4Error error;
            Assert.IsTrue(Native.c4db_rekey(_db, null, &error));

            C4RawDocument *doc = Native.c4raw_get(_db, "test", "key", &error);
            Assert.IsTrue(doc != null);
        }
        
    }
}

