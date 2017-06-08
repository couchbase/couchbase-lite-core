#if LITECORE_PACKAGED
    internal
#else
    public
#endif 
    unsafe struct C4SocketFactory
    {
        private byte providesWebSockets;
        public IntPtr open;
        public IntPtr write;
        public IntPtr completedReceive;
        public IntPtr close;
        private IntPtr requestClose; // unused in .NET

        public C4SocketFactory(SocketOpenDelegate open, SocketCloseDelegate close, SocketWriteDelegate write, SocketCompletedReceiveDelegate completedReceive)
        {
            this.open = Marshal.GetFunctionPointerForDelegate(open);
            this.write = Marshal.GetFunctionPointerForDelegate(write);
            this.completedReceive = Marshal.GetFunctionPointerForDelegate(completedReceive);
            this.close = Marshal.GetFunctionPointerForDelegate(close);
            this.requestClose = IntPtr.Zero;
            this.providesWebSockets = 0;
        }
    }
