using System;
using System.Diagnostics;
using Xunit.Abstractions;

namespace LiteCore.Tests.Util
{
    public static class StopwatchExt
    {
        public static void PrintReport(this Stopwatch st, string what, uint count, string item, ITestOutputHelper output)
        {
            st.Stop();
            var ms = st.Elapsed.TotalMilliseconds;
            #if !DEBUG
            output.WriteLine($"{what} took {ms:F3} ms for {count} {item}s ({{0:F3}} us/{item}, or {{1:F0}} {item}s/sec)",
            ms / (double)count * 1000.0, (double)count / ms * 1000.0);
            #else
            output.WriteLine($"{what}; {count} {item}s (took {ms:F3} ms, but this is UNOPTIMIZED CODE)");
            #endif
        }
    }
}