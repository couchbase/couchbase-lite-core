    public unsafe struct C4ReduceFunction
    {
        public IntPtr accumulate;
        public IntPtr reduce;
        public void* context;

        public C4ReduceFunction(AccumulateDelegate accumulate, ReduceDelegate reduce, void* context)
        {
            this.accumulate = Marshal.GetFunctionPointerForDelegate(accumulate);
            this.reduce = Marshal.GetFunctionPointerForDelegate(reduce);
            this.context = context;
        }
    }