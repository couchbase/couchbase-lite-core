using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
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
    public unsafe class CollatedQueryTest : QueryTestBase
    {

        protected override string JsonPath => "C/tests/data/iTunesMusicLibrary.json";

#if !WINDOWS_UWP
        public CollatedQueryTest(ITestOutputHelper output) : base(output)
        {

        }
#endif

#if !__ANDROID__ // TODO
        [Fact]
        public void TestDBQueryCollated()
        {
#if NETCOREAPP1_0
            if (RuntimeInformation.IsOSPlatform(OSPlatform.Linux)) {
                Console.WriteLine("Linux support not finished yet");
                return;
            }
#endif

            RunTestVariants(() =>
            {
                CompileSelect(Json5("{WHAT: [ ['COLLATE', {'unicode': true, 'case': false, 'diacritic': false}, " +
                                    "['.Artist']] ], " +
                                    "DISTINCT: true, " +
                                    "ORDER_BY: [ [ 'COLLATE', {'unicode': true, 'case': false, 'diacritic': false}, " +
                                    "['.Artist']] ]}"));

                var artists = new List<string>();
                var e = (C4QueryEnumerator*) LiteCoreBridge.Check(err =>
                {
                    var options = C4QueryOptions.Default;
                    return Native.c4query_run(_query, &options, null, err);
                });

                C4Error error;
                while (Native.c4queryenum_next(e, &error)) {
                    var artist = Native.FLValue_AsString(Native.FLArrayIterator_GetValueAt(&e->columns, 0));
                    artists.Add(artist);
                }

                try {
                    error.code.Should().Be(0);
                    artists.Count.Should().Be(2097, "because that is the number of distinct artists in the file");

                    // Benoît Pioulard appears twice in the database, once miscapitalized as BenoÎt Pioulard.
                    // Check that these got coalesced by the DISTINCT operator:
                    artists[214].Should().Be("Benny Goodman");
                    artists[215].Should().Be("Benoît Pioulard");
                    artists[216].Should().Be("Bernhard Weiss");

                    // Make sure "Zoë Keating" sorts correctly:
                    artists[2082].Should().Be("ZENИTH (feat. saåad)");
                    artists[2083].Should().Be("Zoë Keating");
                    artists[2084].Should().Be("Zola Jesus");
                } finally {
                    Native.c4queryenum_free(e);
                }
            });
        }
#endif
    }
}
