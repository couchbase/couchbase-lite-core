using Xunit;

namespace LiteCore.Tests
{
    public unsafe class DocumentTest : Test
    {
        [Fact]
        public void TestFleeceDocs()
        {
            RunTestVariants(() => {
                ImportJSONLines("../C/tests/names_100.json");
            });
        }
    }
}