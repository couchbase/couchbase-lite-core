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
using System;
using System.IO;
using NUnit.Framework;
using System.Runtime.InteropServices;

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
            Assert.AreEqual(fdb_status.RESULT_KEY_NOT_FOUND, (fdb_status)error.code);
            
            // Now get the doc with mustExist=false, which returns an empty doc:
            doc = Native.c4doc_get(_db, DOC_ID, false, &error);
            Assert.IsTrue(doc != null);
            Assert.AreEqual((C4DocumentFlags)0, doc->flags);
            Assert.IsTrue(doc->docID.Equals(DOC_ID));
            Assert.IsTrue(doc->revID.buf == null);
            Assert.IsTrue(doc->selectedRev.revID.buf == null);
            
            using(var t = new TransactionHelper(_db)) {
                Assert.IsTrue(Native.c4doc_insertRevision(doc, REV_ID, BODY, false, false, false, &error));
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
        }
        
        [Test]
        public void TestCreateMultipleRevisions()
        {
            const string REV_2_ID = "2-d00d3333";
            const string BODY_2 = "{\"ok\":\"go\"}";
            CreateRev(DOC_ID, REV_ID, BODY);
            CreateRev(DOC_ID, REV_2_ID, BODY_2);
            
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
            Assert.IsTrue(Native.c4doc_loadRevisionBody(doc, &error)); // have to explicitly load the body
            Assert.IsTrue(doc->selectedRev.body.Equals(BODY));
            Assert.IsFalse(Native.c4doc_selectParentRevision(doc));
        }
        
        [Test]
        public void TestAllDocs()
        {
            for(int i = 1; i < 100; i++) {
                var docID = String.Format("doc-{0,3}", i.ToString("D3"));
                CreateRev(docID, REV_ID, BODY);
            }
            
            C4Error error;
            C4Document* doc;
            
            var options = C4AllDocsOptions.DEFAULT;
            options.includeBodies = false;
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
            var options = C4ChangesOptions.DEFAULT;
            options.includeBodies = false;
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
    }
}

