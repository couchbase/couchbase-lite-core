using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.CompilerServices;
using System.Text;
#if !WINDOWS_UWP
using Xunit;
using Xunit.Abstractions;
#else
using Microsoft.VisualStudio.TestTools.UnitTesting;
using Fact = Microsoft.VisualStudio.TestTools.UnitTesting.TestMethodAttribute;
#endif

namespace LiteCore.Tests
{
#if WINDOWS_UWP
    [TestClass]
#endif
    public abstract class TestBase
    {
#if !WINDOWS_UWP
        protected readonly ITestOutputHelper _output;
#else
        protected TestContext _output;

        public virtual TestContext TestContext
        {
            get => _output;
            set => _output = value;
        }
#endif
        private StringBuilder _sb = new StringBuilder();

        protected abstract int NumberOfOptions { get; }

        protected Exception CurrentException { get; private set; }

        protected abstract void SetupVariant(int option);
        protected abstract void TeardownVariant(int option);

#if !WINDOWS_UWP
        protected TestBase(ITestOutputHelper output)
        {
            _output = output;
        }
#endif

        protected void WriteLine(string line = "")
        {
            _output.WriteLine($"{_sb.ToString()}{line}");
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
