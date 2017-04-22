using LiteCore.Interop;
using FluentAssertions;

using System.Collections.Generic;
#if !WINDOWS_UWP
using Xunit;
using Xunit.Abstractions;
#else
using Fact = Microsoft.VisualStudio.TestTools.UnitTesting.TestMethodAttribute;
#endif

namespace LiteCore.Tests
{
#if WINDOWS_UWP
    [Microsoft.VisualStudio.TestTools.UnitTesting.TestClass]
#endif
    public unsafe abstract class QueryTestBase : Test
    {
        internal C4Query *_query;

        protected abstract string JsonPath
        {
            get;
        }

#if !WINDOWS_UWP
        protected QueryTestBase(ITestOutputHelper output) : base(output)
        {

        }
#endif

        protected IList<string> Run(ulong skip = 0, ulong limit = ulong.MaxValue, string bindings = null)
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

        protected string Json5(string input)
        {
            FLError err;
            var json = Native.FLJSON5_ToJSON(input, &err);
            json.Should().NotBeNull("because otherwise the JSON failed to parse");
            return json;
        }

        internal C4Query* Compile(string whereExpr, string sortExpr = null)
        {
            string queryString = whereExpr;
            if(sortExpr != null) {
                queryString = $"[\"SELECT\", {{\"WHERE\": {whereExpr}, \"ORDER_BY\": {sortExpr}}}]";
            } 

            Native.c4query_free(_query);
            _query = (C4Query *)LiteCoreBridge.Check(err => Native.c4query_new(Db, queryString, err));
            return _query;
        }

        protected override void SetupVariant(int option)
        {
            base.SetupVariant(option);
            ImportJSONLines(JsonPath);
        }

        protected override void TeardownVariant(int option)
        {
            Native.c4query_free(_query);
            _query = null;
            base.TeardownVariant(option);
        }
    }
}