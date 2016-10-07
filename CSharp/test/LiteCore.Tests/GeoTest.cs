using System;
using System.IO;
using FluentAssertions;
using LiteCore.Interop;
using LiteCore.Util;
using Xunit;

namespace LiteCore.Tests
{
    public unsafe class GeoTest : Test
    {
        private C4View* _view;

        [Fact]
        public void TestCreateIndex()
        {
            RunTestVariants(() => {
                CreateDocs(100);
                CreateIndex();
            });
        }

        private static double RandomLat(Random r)
        {
            return r.NextDouble() * 180.0 - 90.0;
        }

        private static double RandomLon(Random r)
        {
            return r.NextDouble() * 360.0 - 180.0;
        }

        private void CreateDocs(uint n, bool verbose = false)
        {
            var rng = new Random(42);
            LiteCoreBridge.Check(err => Native.c4db_beginTransaction(Db, err));
            try {
                for(uint i = 0; i < n; ++i) {
                    var docID = i.ToString();
                    var lat0 = RandomLat(rng);
                    var lon0 = RandomLon(rng);
                    var lat1 = Math.Min(lat0 + 0.5, 90.0);
                    var lon1 = Math.Min(lon0 + 0.5, 180.0);
                    var body = $"({lon0}, {lat0}, {lon1}, {lat1})";

                    using(var docID_ = new C4String(docID))
                    using(var body_ = new C4String(body)) {
                        var rq = new C4DocPutRequest {
                            docID = docID_.AsC4Slice(),
                            body = body_.AsC4Slice(),
                            save = true
                        };

                        var doc = (C4Document *)LiteCoreBridge.Check(err => {
                            var localRq = rq;
                            return Native.c4doc_put(Db, &localRq, null, err);
                        });

                        if(verbose) {
                            Console.WriteLine($"Added {docID} --> {body}");
                        }

                        Native.c4doc_free(doc);
                    }
                }
            } finally {
                LiteCoreBridge.Check(err => Native.c4db_endTransaction(Db, true, err));
            }
        }

        private void CreateIndex()
        {
            var ind = (C4Indexer *)LiteCoreBridge.Check(err => Native.c4indexer_begin(Db, 
                new[] { _view }, err));
            var e = (C4DocEnumerator *)LiteCoreBridge.Check(err => Native.c4indexer_enumerateDocuments(ind, err));

            C4Document *doc;
            C4Error error;
            while(null != (doc = Native.c4enum_nextDocument(e, &error))) {
                var body = doc->selectedRev.body.CreateString();
                var components = body.Trim('(', ')').Split(',');
                var area = new C4GeoArea();
                area.xmin = Double.Parse(components[0]);
                area.ymin = Double.Parse(components[1]);
                area.xmax = Double.Parse(components[2]);
                area.ymax = Double.Parse(components[3]);
                var keys = new C4Key*[1];
                var values = new C4Slice[1];
                keys[0] = Native.c4key_newGeoJSON("{\"geo\":true}", area);
                values[0] = C4Slice.Constant("1234");
                LiteCoreBridge.Check(err => Native.c4indexer_emit(ind, doc, 0, keys, values, err));
                Native.c4key_free(keys[0]);
                Native.c4doc_free(doc);
            }

            Native.c4enum_free(e);
            error.Code.Should().Be(0, "because otherwise an error occurred somewhere");
            LiteCoreBridge.Check(err => Native.c4indexer_end(ind, true, err));
        }

        [Fact]
        public void TestQuery()
        {
           RunTestVariants(() => {
               CreateDocs(100);
               CreateIndex();

               var queryArea = new C4GeoArea {
                   xmin = 10,
                   ymin = 10,
                   xmax = 40,
                   ymax = 40
               };

               var e = (C4QueryEnumerator *)LiteCoreBridge.Check(err => Native.c4view_geoQuery(_view,
                    queryArea, err));

                uint found = 0;
                C4Error error;
                while(Native.c4queryenum_next(e, &error)) {
                    ++found;
                    var a = e->geoBBox;
                    var expected = C4Slice.Constant("1234");
                    e->value.Equals(expected).Should().BeTrue("because the value should be correct");
                    a.xmin.Should().BeLessOrEqualTo(40, "because otherwise it is outside the specified area");
                    a.xmax.Should().BeGreaterOrEqualTo(10, "because otherwise it is outside the specified area");
                    a.ymin.Should().BeLessOrEqualTo(40, "because otherwise it is outside the specified area");
                    a.ymax.Should().BeGreaterOrEqualTo(10, "because otherwise it is outside the specified area");

                    expected = C4Slice.Constant("{\"geo\":true}");
                    e->geoJSON.Equals(expected).Should().BeTrue("because the geo JSON should match what was stored");
                }

                Native.c4queryenum_free(e);
                error.Code.Should().Be(0, "because otherwise an error occurred somewhere");
                found.Should().Be(1, "because that is how many entries fall in the given area");
           });
        }
        
        protected override void SetupVariant(int option)
        {
            base.SetupVariant(option);

            Native.c4view_deleteByName(Db, "geoview", null);
            _view = (C4View *)LiteCoreBridge.Check(err => Native.c4view_open(Db, null, "geoview", "1",
                Native.c4db_getConfig(Db), err));
        }

        protected override void TeardownVariant(int option)
        {
            if(_view != null) {
                LiteCoreBridge.Check(err => Native.c4view_delete(_view, err));
            }

            Native.c4view_free(_view);

            base.TeardownVariant(option);
        }
    }
}