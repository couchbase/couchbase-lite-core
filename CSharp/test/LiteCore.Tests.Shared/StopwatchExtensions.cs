
using System;
using System.Diagnostics;
#if !WINDOWS_UWP
using Xunit.Abstractions;
#else 
using Microsoft.VisualStudio.TestTools.UnitTesting;
#endif

namespace LiteCore.Tests.Util
{
    public static class StopwatchExt
    {
#if !WINDOWS_UWP
        public static void PrintReport(this Stopwatch st, string what, uint count, string item, ITestOutputHelper output)
#else
        public static void PrintReport(this Stopwatch st, string what, uint count, string item, TestContext output)
#endif
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