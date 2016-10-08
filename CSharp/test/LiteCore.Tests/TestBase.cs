using System;
using System.Runtime.CompilerServices;
using Xunit;

[assembly: CollectionBehavior(DisableTestParallelization = true)]

namespace LiteCore.Tests
{
    public abstract class TestBase
    {
        protected abstract int NumberOfOptions { get; }

        protected Exception CurrentException { get; private set; }

        protected abstract void SetupVariant(int option);
        protected abstract void TeardownVariant(int option);

        protected void RunTestVariants(Action a, [CallerMemberName]string caller = null)
        {
            Console.WriteLine($"Begin {caller}");
            for(int i = 0; i < NumberOfOptions; i++) {
                CurrentException = null;
                SetupVariant(i);
                try {
                    a();
                } catch(Exception e) {
                    CurrentException = e;
                } finally {
                    try {
                        Console.WriteLine("Finished variant");
                        TeardownVariant(i);
                    } catch(Exception e) {
                        Console.WriteLine($"Warning: error tearing down {e}");
                    }
                }
            }
            Console.WriteLine($"End {caller}");
        }
    }
}