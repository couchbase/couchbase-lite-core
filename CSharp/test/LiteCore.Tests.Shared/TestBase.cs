using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.CompilerServices;
using System.Text;
using Xunit;
using Xunit.Abstractions;

[assembly: CollectionBehavior(DisableTestParallelization = true)]

namespace LiteCore.Tests
{
    public abstract class TestBase
    {
        protected readonly ITestOutputHelper _output;
        private StringBuilder _sb = new StringBuilder();

        protected abstract int NumberOfOptions { get; }

        protected Exception CurrentException { get; private set; }

        protected abstract void SetupVariant(int option);
        protected abstract void TeardownVariant(int option);

        protected TestBase(ITestOutputHelper output)
        {
            _output = output;
        }

        protected void WriteLine(string line = "")
        {
            _output.WriteLine($"{_sb.ToString()}{line}");
            _sb.Clear();
        }

        protected void Write(string str)
        {
            _sb.Append(str);
        }

        protected void RunTestVariants(Action a, [CallerMemberName]string caller = null)
        {
            var exceptions = new ConcurrentDictionary<int, List<Exception>>();
            WriteLine($"Begin {caller}");
            for(int i = 0; i < NumberOfOptions; i++) {
                CurrentException = null;
                SetupVariant(i);
                try {
                    a();
                } catch(Exception e) {
                    CurrentException = e;
                    throw;
                } finally {
                    try {
                        WriteLine("Finished variant");
                        TeardownVariant(i);
                    } catch(Exception e) {
                        WriteLine($"Warning: error tearing down {e}");
                    }
                }
            }
        }
    }
}