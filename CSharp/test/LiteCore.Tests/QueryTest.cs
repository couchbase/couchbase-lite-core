using LiteCore.Interop;
using FluentAssertions;
using Xunit;
using System.Collections.Generic;

namespace LiteCore.Tests
{
    public unsafe class QueryTest : Test
    {
        private C4Query *_query;

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

#if false
                Compile("{\"contact.phone\": {\"$elemMatch\": {\"$like\": \"%97%\"}}}");
                Run().Should().Equal(new[] { "0000013", "0000014", "0000027", "0000029", "0000045", "0000048", 
                "0000070", "0000085", "0000096" }, "because otherwise the query returned incorrect results");
#endif

                Compile(Json5("['AND', ['=', ['count()', ['.', 'contact', 'phone']], 2]," +
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
        public void TestFullTextQuery()
        {
            RunTestVariants(() => {
                LiteCoreBridge.Check(err => Native.c4db_createIndex(Db, "contact.address.street", 
                C4IndexType.FullTextIndex, null, err));
                Compile(Json5("['MATCH', ['.', 'contact', 'address', 'street'], 'Hwy']"));
                Run().Should().Equal(new[] { "0000013", "0000015", "0000043", "0000044", "0000052" }, 
                "because otherwise the query returned incorrect results");
            });
        }

        private IList<string> Run(ulong skip = 0, ulong limit = ulong.MaxValue, string bindings = null)
        {
            ((long)_query).Should().NotBe(0, "because otherwise what are we testing?");
            var docIDs = new List<string>();
            var options = C4QueryOptions.Default;
            options.skip = skip;
            options.limit = limit;
            var e = (C4QueryEnumerator*)LiteCoreBridge.Check(err => {
                var localOptions = options;
                return Native.c4query_run(_query, &localOptions, bindings, err);
            });

            C4Error error;
            while(Native.c4queryenum_next(e, &error)) {
                docIDs.Add(e->docID.CreateString());
            }

            error.code.Should().Be(0, "because otherwise an error occurred during enumeration");
            Native.c4queryenum_free(e);
            return docIDs;
        }

        private string Json5(string input)
        {
            FLError err;
            var json = Native.FLJSON5_ToJSON(input, &err);
            json.Should().NotBeNull("because otherwise the JSON failed to parse");
            return json;
        }

        private C4Query* Compile(string whereExpr, string sortExpr = null)
        {
            string queryString = whereExpr;
            if(sortExpr != null) {
                queryString = $"[\"SELECT\", {{\"WHERE\": {whereExpr}, \"ORDER BY\": {sortExpr}}}]";
            } 

            Native.c4query_free(_query);
            _query = (C4Query *)LiteCoreBridge.Check(err => Native.c4query_new(Db, queryString, err));
            return _query;
        }

        protected override void SetupVariant(int option)
        {
            base.SetupVariant(option);
            ImportJSONLines("../../../C/tests/data/names_100.json");
        }

        protected override void TeardownVariant(int option)
        {
            Native.c4query_free(_query);
            _query = null;
            base.TeardownVariant(option);
        }
    }
}