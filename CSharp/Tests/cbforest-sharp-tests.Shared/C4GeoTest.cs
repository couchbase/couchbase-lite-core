//
// C4GeoTest.cs
//
// Author:
// 	Jim Borden  <jim.borden@couchbase.com>
//
// Copyright (c) 2016 Couchbase, Inc All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
using System;
using NUnit.Framework;
using System.IO;

namespace CBForest.Tests
{
    [TestFixture]
    public unsafe class C4GeoTest : C4Test
    {
        private static readonly string VIEW_INDEX_PATH = 
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "forest_temp_view.index");
        
        private C4View *_view;
        private static Random _R;
        
        public override void SetUp()
        {
            base.SetUp();
            File.Delete(VIEW_INDEX_PATH);
            C4Error error;
            _view = Native.c4view_open(_db, VIEW_INDEX_PATH, "myview", "1", C4DatabaseFlags.Create, EncryptionKey, &error);
            Assert.IsTrue(_view != null);
        }
        
        public override void TearDown()
        {
            C4Error error;
            if(_view != null) {
                Native.c4view_delete(_view, &error);   
            }
            
            base.TearDown();
        }
        
        [Test]
        public void TestCreateIndex()
        {
            CreateDocs(100);
            CreateIndex();
        }
        
        [Test]
        public void TestQuery()
        {
            const bool verbose = true;
            CreateDocs(100, verbose);
            CreateIndex();
            
            C4GeoArea area = new C4GeoArea(10, 10, 60, 60);
            C4Error error;
            C4QueryEnumerator *e = Native.c4view_geoQuery(_view, area, &error);
            Assert.IsTrue(e != null);
            
            int found = 0;
            foreach(var doc in new CBForestQueryEnumerator(e)) {
                ++found;
                var a = doc.BoundingBox;
                Assert.IsTrue(doc.Value.Equals("1234"));
                Assert.IsTrue(a.xmin <= 60 && a.xmax >= 10 && a.ymin <= 60 && a.ymax >= 10);
                Assert.IsTrue(doc.GeoJSONRaw.Equals("{\"geo\":true}"));
            }
            
            Assert.AreEqual(0, error.code);
            Assert.AreEqual(5, found);
        }
        
        private static double RandomLatitude()
        {
            return _R.NextDouble() * 180.0 - 90.0;
        }
        
        private static double RandomLongitude()
        {
            return _R.NextDouble() * 360.0 - 180.0;
        }
        
        private void CreateDocs(uint n, bool verbose = false)
        {
            _R = new Random(42);
            using(var t = new TransactionHelper(_db)) {
                for(int i = 0; i < n; i++) {
                    var docID = "doc-" + i.ToString();
                    double lat0 = RandomLatitude();
                    double lon0 = RandomLongitude();
                    double lat1 = Math.Min(lat0 + 0.5, 90.0);
                    double lon1 = Math.Min(lon0 + 0.5, 180.0);
                    var body = String.Format("({0}, {1}, {2}, {3})", lon0, lat0, lon1, lat1);
                    
                    var rq = new C4DocPutRequest();
                    rq.docID = docID;
                    rq.body = body;
                    rq.save = true;
                    C4Error error;
                    var doc = Native.c4doc_put(_db, rq, null, &error);
                    Assert.IsTrue(doc != null);
                    if(verbose) {
                        Console.WriteLine("Added {0} -> {1}", docID, body);   
                    }
                }
            }
        }
        
        private void CreateIndex()
        {
            C4Error error;
            C4Indexer* ind = Native.c4indexer_begin(_db, new C4View*[] { _view }, &error);
            Assert.IsTrue(ind != null);
            
            foreach(var doc in new CBForestDocEnumerator(ind)) {
                var body = (string)doc.GetDocument()->selectedRev.body;
                var pieces = body.Split(',');
                C4GeoArea area;
                Assert.IsTrue(Double.TryParse(pieces[0].TrimStart('('), out area.xmin));
                Assert.IsTrue(Double.TryParse(pieces[1], out area.ymin));
                Assert.IsTrue(Double.TryParse(pieces[2], out area.xmax));
                Assert.IsTrue(Double.TryParse(pieces[3].TrimEnd(')'), out area.ymax));
                var keys = new C4Key*[] { Native.c4key_newGeoJSON("{\"geo\":true}", area) };
                Assert.IsTrue(Native.c4indexer_emit(ind, doc.GetDocument(), 0, keys, new[] { "1234" }, &error));
                Native.c4key_free(keys[0]);
            }
            
            Assert.AreEqual(0, error.code);
            Assert.IsTrue(Native.c4indexer_end(ind, true, &error));
        }
    }
    
}

