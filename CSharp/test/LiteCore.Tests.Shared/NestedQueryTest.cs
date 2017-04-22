using FluentAssertions;
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
    public unsafe class NestedQueryTest : QueryTestBase
    {
        protected override string JsonPath 
        {
            get {
                return "C/tests/data/nested.json";
            }
        }

#if !WINDOWS_UWP
        public NestedQueryTest(ITestOutputHelper output) : base(output)
        {

        }
#endif

        [Fact]
        public void TestDBQueryAnyNested()
        {
            RunTestVariants(() => {
                Compile(Json5("['ANY', 'Shape', ['.', 'shapes'], ['=', ['?', 'Shape', 'color'], 'red']]"));
                Run().Should().Equal(new[] { "0000001", "0000003" }, "because otherwise the query returned incorrect results");
            });
        }
    }
}