//
// C4Socket.cs
//
// Copyright (c) 2017 Couchbase, Inc All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using ObjCRuntime;

namespace LiteCore.Interop
{

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]   
#if LITECORE_PACKAGED
    internal
#else
    public
#endif
        unsafe delegate void SocketOpenDelegate(C4Socket* socket, C4Address* address, C4Slice options);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
#if LITECORE_PACKAGED
    internal
#else
    public
#endif
        unsafe delegate void SocketCloseDelegate(C4Socket* socket);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
#if LITECORE_PACKAGED
    internal
#else
    public
#endif 
        unsafe delegate void SocketRequestCloseDelegate(C4Socket* socket, int status, C4Slice message);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
#if LITECORE_PACKAGED
    internal
#else
    public
#endif 
        unsafe delegate void SocketWriteDelegate(C4Socket* socket, C4SliceResult allocatedData);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
#if LITECORE_PACKAGED
    internal
#else
    public
#endif 
        unsafe delegate void SocketCompletedReceiveDelegate(C4Socket* socket, UIntPtr byteCount);

#if LITECORE_PACKAGED
	internal
#else
    public
#endif
		unsafe delegate void SocketRequestCloseDelegateManaged(C4Socket* socket, int status, string message);

#if LITECORE_PACKAGED
    internal
#else
    public
#endif 
        unsafe delegate void SocketWriteDelegateManaged(C4Socket* socket, byte[] allocatedData);
    
#if LITECORE_PACKAGED
    internal
#else
    public
#endif 
        unsafe delegate void SocketCompletedReceiveDelegateManaged(C4Socket* socket, ulong byteCount);

#if LITECORE_PACKAGED
    internal
#else
    public
#endif 
        unsafe delegate void SocketErrorDelegate(C4Socket* socket, Exception e);

#if LITECORE_PACKAGED
    internal
#else
    public
#endif
        unsafe static class SocketFactory
    {
        private static readonly SocketOpenDelegate _open;
        private static readonly SocketCloseDelegate _close;
        private static readonly SocketWriteDelegate _write;
        private static readonly SocketCompletedReceiveDelegate _completedReceive;

        private static SocketOpenDelegate _externalOpen;
        private static SocketCloseDelegate _externalClose;
        private static SocketWriteDelegateManaged _externalWrite;
        private static SocketCompletedReceiveDelegateManaged _externalCompletedReceive;
        private static SocketErrorDelegate _error;

        private static C4SocketFactory InternalFactory { get; }

        static SocketFactory()
        {
            _open = SocketOpened;
            _close = SocketClose;
            _write = SocketWrittenTo;
            _completedReceive = SocketCompletedReceive;
            InternalFactory = new C4SocketFactory(_open, _close, _write, _completedReceive);
            Native.c4socket_registerFactory(InternalFactory);
        }

        public static void RegisterFactory(SocketOpenDelegate doOpen, SocketCloseDelegate doClose, 
            SocketWriteDelegateManaged doWrite, SocketCompletedReceiveDelegateManaged doCompleteReceive)
        {
            _externalOpen = doOpen;
            _externalClose = doClose;
            _externalWrite = doWrite;
            _externalCompletedReceive = doCompleteReceive;
        }

        public static void SetErrorHandler(SocketErrorDelegate doError)
        {
            _error = doError;
        }

        [MonoPInvokeCallback(typeof(SocketOpenDelegate))]
        private static void SocketOpened(C4Socket* socket, C4Address* address, C4Slice options)
        {
            try {
                _externalOpen?.Invoke(socket, address, options);
            } catch (Exception e) {
                _error?.Invoke(socket, new Exception("Error opening to socket", e));
                Native.c4socket_closed(socket, new C4Error(C4ErrorCode.UnexpectedError));
            }
        }

        [MonoPInvokeCallback(typeof(SocketCloseDelegate))]
        private static void SocketClose(C4Socket* socket)
        {
            try {
				_externalClose?.Invoke(socket);
            } catch (Exception e) {
                _error?.Invoke(socket, new Exception("Error closing socket", e));
            }
        }

        [MonoPInvokeCallback(typeof(SocketWriteDelegate))]
        private static void SocketWrittenTo(C4Socket* socket, C4SliceResult allocatedData)
        {
            try {
                _externalWrite?.Invoke(socket, ((C4Slice) allocatedData).ToArrayFast());
            } catch (Exception e) {
                _error?.Invoke(socket, new Exception("Error writing to socket", e));
                Native.c4socket_closed(socket, new C4Error(C4ErrorCode.UnexpectedError));
            } finally {
                allocatedData.Dispose();
            }
        }

        [MonoPInvokeCallback(typeof(SocketCompletedReceiveDelegate))]
        private static void SocketCompletedReceive(C4Socket* socket, UIntPtr byteCount)
        {
            try {
                _externalCompletedReceive?.Invoke(socket, byteCount.ToUInt64());
            } catch (Exception e) {
                _error?.Invoke(socket, new Exception("Error completing receive for socket", e));
                Native.c4socket_closed(socket, new C4Error(C4ErrorCode.UnexpectedError));
            }
        }
    }

#if LITECORE_PACKAGED
    internal
#else
    public
#endif
        static unsafe partial class Native
    {
        public static void c4socket_gotHTTPResponse(C4Socket* socket, int httpStatus,
            IDictionary<string, object> headers)
        {
            using (var headers_ = headers.FLEncode()) {
                c4socket_gotHTTPResponse(socket, httpStatus, (C4Slice)headers_);
            }
        }
    }
}
