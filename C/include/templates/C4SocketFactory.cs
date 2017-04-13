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
        private IntPtr close; // unused in .NET
        public IntPtr requestClose;

        public C4SocketFactory(SocketOpenDelegate open, SocketRequestCloseDelegate requestClose, SocketWriteDelegate write, SocketCompletedReceiveDelegate completedReceive)
        {
            this.open = Marshal.GetFunctionPointerForDelegate(open);
            this.write = Marshal.GetFunctionPointerForDelegate(write);
            this.completedReceive = Marshal.GetFunctionPointerForDelegate(completedReceive);
            this.close = IntPtr.Zero;
            this.requestClose = Marshal.GetFunctionPointerForDelegate(requestClose);
            this.providesWebSockets = 1;
        }
    }
