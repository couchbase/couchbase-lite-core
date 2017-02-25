using FluentAssertions;
using LiteCore.Interop;
using Xunit;
using Xunit.Abstractions;

namespace LiteCore.Tests
{
    public unsafe class KeyTest : TestBase
    {
        private C4Key* _key;

        public KeyTest(ITestOutputHelper output) : base(output)
        {

        }

        protected override int NumberOfOptions
        {
            get {
                return 1;
            }
        }

        [Fact]
        public void TestCreateKey()
        {
            RunTestVariants(() => {
                var reader = Native.c4key_read(_key);
                Native.c4key_toJSON(&reader).Should().Be("[null,false,true,0,12345,-2468,\"foo\",[]]",
                    "because otherwise the key saved incorrectly");
            });
        }

        [Fact]
        public void TestReadKey()
        {
            RunTestVariants(() => {
                var r = Native.c4key_read(_key);
                Native.c4key_peek(&r).Should().Be(C4KeyToken.Array, "because that is what comes next");
                Native.c4key_skipToken(&r);
                Native.c4key_peek(&r).Should().Be(C4KeyToken.Null, "because that is what comes next");
                Native.c4key_skipToken(&r);
                Native.c4key_peek(&r).Should().Be(C4KeyToken.Bool, "because that is what comes next");
                Native.c4key_readBool(&r).Should().BeFalse("because that is the next value");
                Native.c4key_readBool(&r).Should().BeTrue("because that is the next value");
                Native.c4key_readNumber(&r).Should().Be(0.0, "because that is the next value");
                Native.c4key_readNumber(&r).Should().Be(12345.0, "because that is the next value");
                Native.c4key_readNumber(&r).Should().Be(-2468.0, "because that is the next value");
                Native.c4key_readString(&r).Should().Be("foo", "because that is the next value");
                Native.c4key_peek(&r).Should().Be(C4KeyToken.Array, "because that is what comes next");
                Native.c4key_skipToken(&r);
                Native.c4key_peek(&r).Should().Be(C4KeyToken.EndSequence, "because that is what comes next");
                Native.c4key_skipToken(&r);
                Native.c4key_peek(&r).Should().Be(C4KeyToken.EndSequence, "because that is what comes next");
                Native.c4key_skipToken(&r);
            });
        }
        
        protected override void SetupVariant(int option)
        {
            _key = Native.c4key_new();
            Native.c4key_beginArray(_key);
            Native.c4key_addNull(_key);
            Native.c4key_addBool(_key, false);
            Native.c4key_addBool(_key, true);
            Native.c4key_addNumber(_key, 0);
            Native.c4key_addNumber(_key, 12345);
            Native.c4key_addNumber(_key, -2468);
            Native.c4key_addString(_key, "foo");
            Native.c4key_beginArray(_key);
            Native.c4key_endArray(_key);
            Native.c4key_endArray(_key);
        }

        protected override void TeardownVariant(int option)
        {
            Native.c4key_free(_key);
        }
    }
}