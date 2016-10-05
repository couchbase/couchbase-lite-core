using System;

namespace LiteCore.Tests
{
    public abstract class TestBase
    {
        protected abstract int NumberOfOptions { get; }

        protected abstract void SetupVariant(int option);
        protected abstract void TeardownVariant(int option);

        protected void RunTestVariants(Action a)
        {
            for(int i = 0; i < NumberOfOptions; i++) {
                SetupVariant(i);
                try {
                    a();
                } finally {
                    try {
                        TeardownVariant(i);
                    } catch(Exception e) {
                        Console.WriteLine($"Warning: error tearing down {e}");
                    }
                }
            }
        }
    }
}