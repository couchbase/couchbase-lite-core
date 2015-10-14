//
//  C4ViewTest.cs
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

namespace CBForest.Tests
{
    [TestFixture]
    public unsafe class C4ViewTest : C4Test
    {
        private C4View *_view;
        private static readonly string VIEW_INDEX_PATH = 
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "forest_temp_view.index");
        
        public override void SetUp()
        {
            base.SetUp();
            C4Error error;
            _view = Native.c4view_open(_db, VIEW_INDEX_PATH, "myview", "1", C4DatabaseFlags.Create, EncryptionKey, &error);
            Assert.IsTrue(_view != null);
        }
        
        public override void TearDown()
        {
            C4Error error;
            Assert.IsTrue(Native.c4view_delete(_view, &error));
            base.TearDown();
        }
        
        [Test]
        public void TestEmptyState()
        {
            Assert.AreEqual(0UL, Native.c4view_getTotalRows(_view));
            Assert.AreEqual(0UL, Native.c4view_getLastSequenceIndexed(_view));
            Assert.AreEqual(0UL, Native.c4view_getLastSequenceChangedAt(_view));
        }
        
        [Test]
        public void TestCreateIndex()
        {
            CreateIndex();
            
            Assert.AreEqual(200UL, Native.c4view_getTotalRows(_view));
            Assert.AreEqual(100UL, Native.c4view_getLastSequenceIndexed(_view));
            Assert.AreEqual(100UL, Native.c4view_getLastSequenceChangedAt(_view));
        }
        
        [Test]
        public void TestQueryIndex()
        {
            CreateIndex();
            
            C4Error error;
            var e = Native.c4view_query(_view, null, &error);
            Assert.IsTrue(e != null);
            
            var i = 0;
            while(Native.c4queryenum_next(e, &error)) {
                ++i;
                var buf = default(string);
                if(i <= 100) {
                    buf = i.ToString();
                    Assert.AreEqual(i, e->docSequence);
                } else {
                    buf = String.Format("\"doc-{0}\"", (i - 100).ToString("D3"));
                    Assert.AreEqual(i - 100, e->docSequence);
                }   
                
                Assert.AreEqual(buf, ToJSON(e->key));
                Assert.IsTrue(e->value.Equals("1234"));
            }
            
            Assert.AreEqual(0, error.code);
            Assert.AreEqual(200, i);
        }
        
        [Test]
        public void TestIndexVersion()
        {
            CreateIndex();
            
            // Reopen view with same version string:
            var error = default(C4Error);
            Assert.IsTrue(Native.c4view_close(_view, &error));
            _view = Native.c4view_open(_db, VIEW_INDEX_PATH, "myview", "1", C4DatabaseFlags.Create, EncryptionKey, &error);
            Assert.IsTrue(_view != null);
            
            Assert.AreEqual(200UL, Native.c4view_getTotalRows(_view));
            Assert.AreEqual(100UL, Native.c4view_getLastSequenceIndexed(_view));
            Assert.AreEqual(100UL, Native.c4view_getLastSequenceChangedAt(_view));
            
            // Reopen view with different version string:
            Assert.IsTrue(Native.c4view_close(_view, &error));
            _view = Native.c4view_open(_db, VIEW_INDEX_PATH, "myview", "2", C4DatabaseFlags.Create, EncryptionKey, &error);
            Assert.IsTrue(_view != null);

            Assert.AreEqual(0UL, Native.c4view_getTotalRows(_view));
            Assert.AreEqual(0UL, Native.c4view_getLastSequenceIndexed(_view));
            Assert.AreEqual(0UL, Native.c4view_getLastSequenceChangedAt(_view));
        }
        
        private void CreateIndex()
        {
            for(int i = 1; i <= 100; i++) {
                var docId = String.Format("doc-{0}", i.ToString("D3"));
                CreateRev(docId, REV_ID, BODY);
            }
            
            C4Error error;
            C4Indexer* ind = Native.c4indexer_begin(_db, new C4View*[] { _view }, &error);
            Assert.IsTrue(ind != null);

            var e = Native.c4indexer_enumerateDocuments(ind, &error);
            Assert.IsTrue(e != null);

            C4Document* doc;
            while(null != (doc = Native.c4enum_nextDocument(e, &error))) {
                // Index 'doc':
                var keys = new C4Key*[] { Native.c4key_new(), Native.c4key_new() };
                var vals = new string[] { "1234", "1234" };
                Native.c4key_addString(keys[0], doc->docID);
                Native.c4key_addNumber(keys[1], doc->sequence);
                Assert.IsTrue(Native.c4indexer_emit(ind, doc, 0, keys, vals, &error));
                Native.c4key_free(keys[0]);
                Native.c4key_free(keys[1]);
            }

            Assert.AreEqual(0, error.code);
            Assert.IsTrue(Native.c4indexer_end(ind, true, &error));
        }
    }
}

