#if LITECORE_PACKAGED
    internal
#else
    public
#endif 
    unsafe struct C4SocketFactory
    {
        public IntPtr open;
        public IntPtr close;
        public IntPtr write;
        public IntPtr completedReceive;

        public C4SocketFactory(SocketOpenDelegate open, SocketCloseDelegate close, SocketWriteDelegate write, SocketCompletedReceiveDelegate completedReceive)
        {
            this.open = Marshal.GetFunctionPointerForDelegate(open);
            this.close = Marshal.GetFunctionPointerForDelegate(close);
            this.write = Marshal.GetFunctionPointerForDelegate(write);
            this.completedReceive = Marshal.GetFunctionPointerForDelegate(completedReceive);
        }
    }