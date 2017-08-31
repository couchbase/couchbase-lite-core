using FluentAssertions;
using LiteCore.Interop;
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
    public unsafe class PathsQueryTest : QueryTestBase
    {
        protected override string JsonPath => "C/tests/data/paths.json";

#if !WINDOWS_UWP
        public PathsQueryTest(ITestOutputHelper output) : base(output)
        {

        }
#endif

        [Fact]
        public void TestDBQueryAnyWithPaths()
        {
            RunTestVariants(() =>
            {
                // For https://github.com/couchbase/couchbase-lite-core/issues/238
                Compile(Json5("['ANY','path',['.paths'],['=',['?path','city'],'San Jose']]"));
                Run().Should().BeEquivalentTo(new[] {"0000001"});

                Compile(Json5("['ANY','path',['.paths'],['=',['?path.city'],'San Jose']]"));
                Run().Should().BeEquivalentTo(new[] { "0000001" });

                Compile(Json5("['ANY','path',['.paths'],['=',['?path','city'],'Palo Alto']]"));
                Run().Should().BeEquivalentTo(new[] { "0000001", "0000002" });
            });
        }
    }
}
