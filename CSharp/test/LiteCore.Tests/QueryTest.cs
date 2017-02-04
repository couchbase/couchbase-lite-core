using LiteCore.Interop;
using FluentAssertions;
using Xunit;

namespace LiteCore.Tests
{
    public unsafe class QueryTest : QueryTestBase
    {
        protected override string JsonPath
        {
            get {
                return "../../../C/tests/data/names_100.json";
            }
        }

        [Fact]
        public void TestQueryDB()
        {
            RunTestVariants(() => {
                Compile(Json5("['=', ['.', 'contact', 'address', 'state'], 'CA']"));
                Run().Should().Equal(new[] { "0000001", "0000015", "0000036", "0000043", "0000053", "0000064", 
                "0000072", "0000073" }, "because otherwise the query returned incorrect results");
                Run(1, 8).Should().Equal(new[] { "0000015", "0000036", "0000043", "0000053", "0000064", 
                "0000072", "0000073" }, "because otherwise the query returned incorrect results");
                Run(1, 4).Should().Equal(new[] { "0000015", "0000036", "0000043", "0000053" }, 
                "because otherwise the query returned incorrect results");

                Compile(Json5("['AND', ['=', ['array_count()', ['.', 'contact', 'phone']], 2]," +
                           "['=', ['.', 'gender'], 'male']]"));
                Run().Should().Equal(new[] { "0000002", "0000014", "0000017", "0000027", "0000031", "0000033", 
                "0000038", "0000039", "0000045", "0000047", "0000049", "0000056", "0000063", "0000065", "0000075", 
                "0000082", "0000089", "0000094", "0000097" }, "because otherwise the query returned incorrect results");
            });
        }

        [Fact]
        public void TestQueryDBSorted()
        {
            RunTestVariants(() => {
                Compile(Json5("['=', ['.', 'contact', 'address', 'state'], 'CA']"), Json5("[['.', 'name', 'last']]"));
                Run().Should().Equal(new[] { "0000015", "0000036", "0000072", "0000043", "0000001", "0000064", 
                "0000073", "0000053" }, "because otherwise the query returned incorrect results");
            });
        }

        [Fact]
        public void TestQueryDBBindings()
        {
            RunTestVariants(() => {
                Compile(Json5("['=', ['.', 'contact', 'address', 'state'], ['$', 1]]"));
                Run(bindings: "{\"1\": \"CA\"}").Should().Equal(new[] { "0000001", "0000015", "0000036", "0000043", 
                "0000053", "0000064", "0000072", "0000073" }, 
                "because otherwise the query returned incorrect results");

                Compile(Json5("['=', ['.', 'contact', 'address', 'state'], ['$', 'state']]"));
                Run(bindings: "{\"state\": \"CA\"}").Should().Equal(new[] { "0000001", "0000015", "0000036", "0000043", 
                "0000053", "0000064", "0000072", "0000073" }, 
                "because otherwise the query returned incorrect results");
            });
        }

        [Fact]
        public void TestDBQueryAny()
        {
            RunTestVariants(() => {
                Compile(Json5("['ANY', 'like', ['.', 'likes'], ['=', ['?', 'like'], 'climbing']]"));
                Run().Should().Equal(new[] { "0000017", "0000021", "0000023", "0000045", "0000060" }, 
                    "because otherwise the query returned incorrect results");

                // This EVERY query has lots of results because every empty `likes` array matches it
                Compile(Json5("['EVERY', 'like', ['.', 'likes'], ['=', ['?', 'like'], 'taxes']]"));
                var results = Run();
                results.Count.Should().Be(42, "because otherwise the query returned incorrect results");
                results[0].Should().Be("0000007", "because otherwise the query returned incorrect results");

                // Changing the op to ANY AND EVERY returns no results
                Compile(Json5("['ANY AND EVERY', 'like', ['.', 'likes'], ['=', ['?', 'like'], 'taxes']]"));
                Run().Should().BeEmpty("because otherwise the query returned incorrect results");

                // Look for people where every like contains an L:
                Compile(Json5("['ANY AND EVERY', 'like', ['.', 'likes'], ['LIKE', ['?', 'like'], '%l%']]"));
                Run().Should().Equal(new[] { "0000017", "0000027", "0000060", "0000068" }, 
                    "because otherwise the query returned incorrect results");
            });
        }

        [Fact]
        public void TestDBQueryExpressionIndex()
        {
            RunTestVariants(() => {
                LiteCoreBridge.Check(err => Native.c4db_createIndex(Db, Json5("[['length()', ['.name.first']]]"), 
                    C4IndexType.ValueIndex, null, err));
                Compile(Json5("['=', ['length()', ['.name.first']], 9]"));
                Run().Should().Equal(new[] { "0000015", "0000099" }, "because otherwise the query returned incorrect results");
            });
        }

        [Fact]
        public void TestDeleteIndexedDoc()
        {
            RunTestVariants(() => {
                LiteCoreBridge.Check(err => Native.c4db_createIndex(Db, Json5("[['length()', ['.name.first']]]"), 
                    C4IndexType.ValueIndex, null, err));
                
                // Delete doc "0000015":
                LiteCoreBridge.Check(err => Native.c4db_beginTransaction(Db, err));
                try {
                    var doc = (C4Document *)LiteCoreBridge.Check(err => Native.c4doc_get(Db, "0000015", true, err));
                    var rq = new C4DocPutRequest {
                        docID = C4Slice.Constant("0000015"),
                        history = &doc->revID,
                        historyCount = 1,
                        revFlags = C4RevisionFlags.Deleted,
                        save = true
                    };
                    var updatedDoc = (C4Document *)LiteCoreBridge.Check(err => {
                        var localRq = rq;
                        return Native.c4doc_put(Db, &localRq, null, err);
                    });

                    Native.c4doc_free(doc);
                    Native.c4doc_free(updatedDoc);
                } finally {
                    LiteCoreBridge.Check(err => Native.c4db_endTransaction(Db, true, err));
                }

                // Now run a query that would have returned the deleted doc, if it weren't deleted:
                Compile(Json5("['=', ['length()', ['.name.first']], 9]"));
                Run().Should().Equal(new[] { "0000099" }, "because otherwise the query returned incorrect results");
            });
        }

        [Fact]
        public void TestFullTextQuery()
        {
            RunTestVariants(() => {
                LiteCoreBridge.Check(err => Native.c4db_createIndex(Db, "[[\".contact.address.street\"]]", 
                C4IndexType.FullTextIndex, null, err));
                Compile(Json5("['MATCH', ['.', 'contact', 'address', 'street'], 'Hwy']"));
                Run().Should().Equal(new[] { "0000013", "0000015", "0000043", "0000044", "0000052" }, 
                "because otherwise the query returned incorrect results");
            });
        }

        [Fact]
        public void TestDBQueryWhat()
        {
            RunTestVariants(() => {
                var expectedFirst = new[] { "Cleveland", "Georgetta", "Margaretta" };
                var expectedLast = new[] { "Bejcek", "Kolding", "Ogwynn" };
                var query = Compile(Json5("{WHAT: ['.name.first', '.name.last'], " +
                            "WHERE: ['>=', ['length()', ['.name.first']], 9]," +
                            "ORDER_BY: [['.name.first']]}"));
                
                var e = (C4QueryEnumerator *)LiteCoreBridge.Check(err => 
                {
                    var localOpts = C4QueryOptions.Default;
                    return Native.c4query_run(query, &localOpts, null, err);
                });

                int i = 0;
                C4Error error;
                while(Native.c4queryenum_next(e, &error)) {
                    var customColumns = Native.c4queryenum_customColumns(e);
                    GetColumn(customColumns, 0).Should().Be(expectedFirst[i], "because otherwise the query returned incorrect results");
                    GetColumn(customColumns, 1).Should().Be(expectedLast[i], "because otherwise the query returned incorrect results");
                    ++i;
                }

                error.code.Should().Be(0, "because otherwise an error occurred during enumeration");
                i.Should().Be(3, "because that is the number of expected rows");
                Native.c4queryenum_free(e);
            });
        }

        [Fact]
        public void TestDBQueryAggregate()
        {
            RunTestVariants(() => {
                Compile(Json5("{WHAT: [['min()', ['.name.last']], ['max()', ['.name.last']]]}"));
                var e = (C4QueryEnumerator *)LiteCoreBridge.Check(err => {
                    var opts = C4QueryOptions.Default;
                    return Native.c4query_run(_query, &opts, null, err);
                });

                var i = 0;
                C4Error error;
                while(Native.c4queryenum_next(e, &error)) {
                    var customColumns = Native.c4queryenum_customColumns(e);
                    GetColumn(customColumns, 0).Should().Be("Aerni", "because otherwise the query returned incorrect results");
                    GetColumn(customColumns, 1).Should().Be("Zirk", "because otherwise the query returned incorrect results");
                    ++i;
                }

                error.code.Should().Be(0, "because otherwise an error occurred during enumeration");
                i.Should().Be(1, "because there is only one result for the query");
                Native.c4queryenum_free(e);
            });
        }

        private static string GetColumn(byte[] customColumns, uint index)
        {
            customColumns.Should().NotBeNull("because otherwise we got bogus data from the DB");
            var cols = Native.FLValue_FromData(customColumns);
            var colsArray = Native.FLValue_AsArray(cols);
            Native.FLArray_Count(colsArray).Should().BeGreaterOrEqualTo(index + 1, "because otherwise there aren't enough columns");
            return Native.FLValue_AsString(Native.FLArray_Get(colsArray, index));
        }
    }
}
