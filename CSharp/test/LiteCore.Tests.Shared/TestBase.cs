// 
//  TestBase.cs
// 
//  Copyright (c) 2017 Couchbase, Inc All rights reserved.
// 
//  Licensed under the Apache License, Version 2.0 (the "License");
//  you may not use this file except in compliance with the License.
//  You may obtain a copy of the License at
// 
//  http://www.apache.org/licenses/LICENSE-2.0
// 
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.
// 
using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
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
            // StringBuilder is not threadsafe
            lock (_sb) {
                _output.WriteLine($"{_sb}{line}");
            }
        }

        protected void Write(string str)
        { 
            // StringBuilder is not threadsafe
            lock (_sb) {
                _sb.Append(str);
            }
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

#if NETCOREAPP2_0
        [System.Runtime.InteropServices.DllImport("kernel32", CharSet = System.Runtime.InteropServices.CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr LoadLibraryEx(string lpFileName, IntPtr hFile, uint dwFlags);

        internal static void LoadDLL()
        {
            if (System.Runtime.InteropServices.RuntimeInformation.IsOSPlatform(System.Runtime.InteropServices.OSPlatform.Windows)) {
                var foundPath = LoadFromNuget() ?? LoadFromAppContext();
                if (foundPath == null) {
                    throw new DllNotFoundException("Could not find LiteCore.dll!  Nothing is going to work!");
                }

                const uint loadWithAlteredSearchPath = 8;
                var ptr = LoadLibraryEx(foundPath, IntPtr.Zero, loadWithAlteredSearchPath);
                if (ptr == IntPtr.Zero)  {
                    throw new BadImageFormatException("Could not load LiteCore.dll!  Nothing is going to work!");
                }
            }
        }

        internal static string LoadFromNuget()
        {
            var architecture = IntPtr.Size == 4
                ? "x86"
                : "x64";
            var nugetBase = Type.GetType("Couchbase.Lite.Support.NetDesktop, Couchbase.Lite.Support.NetDesktop")?.GetTypeInfo()?.Assembly?.Location;
            if (nugetBase != null) {
                for (int i = 0; i < 3; i++) {
                    nugetBase = System.IO.Path.GetDirectoryName(nugetBase);
                }
            }

            var nugetPath = System.IO.Path.Combine(nugetBase, "runtimes", $"win7-{architecture}", "native", "LiteCore.dll");
            return System.IO.File.Exists(nugetPath) ? nugetPath : null;
        }

        internal static string LoadFromAppContext()
        {
            var directory = System.IO.Path.GetDirectoryName(Assembly.GetExecutingAssembly().Location);

            System.Diagnostics.Debug.Assert(System.IO.Path.IsPathRooted(directory), "directory is not rooted.");
            var architecture = IntPtr.Size == 4
                ? "x86"
                : "x64";

            var dllPath = System.IO.Path.Combine(directory, architecture, "LiteCore.dll");
            var dllPathAsp = System.IO.Path.Combine(directory, "bin", architecture, "LiteCore.dll");
            foreach (var path in new[] { dllPath, dllPathAsp }) {
                var foundPath = System.IO.File.Exists(path) ? path : null;
                if (foundPath != null) {
                    return foundPath;
                }
            }

            return null;
        }
#endif
    }
}
