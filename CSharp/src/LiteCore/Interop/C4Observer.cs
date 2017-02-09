//
// C4Observer.cs
//
// Author:
// 	Jim Borden  <jim.borden@couchbase.com>
//
// Copyright (c) 2016 Couchbase, Inc All rights reserved.
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
using System.Runtime.InteropServices;
using System.Threading;
using ObjCRuntime;

namespace LiteCore.Interop
{
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
#if LITECORE_PACKAGED
    internal
#else
    public
#endif
         unsafe delegate void C4DatabaseObserverCallback(C4DatabaseObserver* observer, void* context);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
#if LITECORE_PACKAGED
    internal
#else
    public
#endif
         unsafe delegate void C4DocumentObserverCallback(C4DocumentObserver* observer, C4Slice docID, ulong sequence, void* context);

#if LITECORE_PACKAGED
    internal
#else
    public
#endif
         unsafe delegate void DatabaseObserverCallback(C4DatabaseObserver* observer, object context);

#if LITECORE_PACKAGED
    internal
#else
    public
#endif
         unsafe delegate void DocumentObserverCallback(C4DocumentObserver* observer, string docID, ulong sequence, object context);

#if LITECORE_PACKAGED
    internal
#else
    public
#endif
         sealed unsafe class DatabaseObserver : IDisposable
    {
        private readonly object _context;
        private readonly DatabaseObserverCallback _callback;
        private readonly C4DatabaseObserverCallback _nativeCallback;
        private GCHandle _self;

        public C4DatabaseObserver* Observer
        {
            get {
                return (C4DatabaseObserver*)_observer;
            }
        }
        private long _observer;

        public DatabaseObserver(C4Database* database, DatabaseObserverCallback callback, object context)
        {
            _context = context;
            _callback = callback;
            _nativeCallback = new C4DatabaseObserverCallback(DBObserverCallback);
            _self = GCHandle.Alloc(this, GCHandleType.Pinned);
            _observer = (long)LiteCoreBridge.Check(err => Native.c4dbobs_create(database, _nativeCallback, GCHandle.ToIntPtr(_self).ToPointer()));
        }

        ~DatabaseObserver()
        {
            Dispose(false);
        }

        [MonoPInvokeCallback(typeof(C4DatabaseObserverCallback))]
        private static void DBObserverCallback(C4DatabaseObserver* observer, void* context)
        {
            var self = GCHandle.FromIntPtr((IntPtr)context);
            var obj = self.Target as DatabaseObserver;
            obj._callback?.Invoke(observer, obj._context);
        }

        private void Dispose(bool disposing)
        {
            var old = (C4DatabaseObserver*)Interlocked.Exchange(ref _observer, 0);
            Native.c4dbobs_free(old);
            _self.Free();
        }

        public void Dispose()
        {
            Dispose(true);
            GC.SuppressFinalize(this);
        }
    }

#if LITECORE_PACKAGED
    internal
#else
    public
#endif
         sealed unsafe class DocumentObserver : IDisposable
    {
        private readonly object _context;
        private readonly DocumentObserverCallback _callback;
        private GCHandle _self;

        public C4DocumentObserver* Observer { get; private set; }

        public DocumentObserver(C4Database* database, string docID, DocumentObserverCallback callback, object context)
        {
            _context = context;
            _callback = callback;
            _self = GCHandle.Alloc(this);
            Observer = (C4DocumentObserver *)LiteCoreBridge.Check(err => Native.c4docobs_create(database, docID, DocObserverCallback, GCHandle.ToIntPtr(_self).ToPointer()));
        }

        [MonoPInvokeCallback(typeof(C4DocumentObserverCallback))]
        private static void DocObserverCallback(C4DocumentObserver* observer, C4Slice docID, ulong sequence, void* context)
        {
            var self = GCHandle.FromIntPtr((IntPtr)context);
            var obj = self.Target as DocumentObserver;
            obj._callback?.Invoke(observer, docID.CreateString(), sequence, obj._context);
        }

        public void Dispose()
        {
            Native.c4docobs_free(Observer);
            Observer = null;
            _self.Free();
        }
    }

#if LITECORE_PACKAGED
    internal
#else
    public
#endif
         static unsafe partial class Native
    {
        public static DatabaseObserver c4dbobs_create(C4Database *db, DatabaseObserverCallback callback, object context)
        {
            return new DatabaseObserver(db, callback, context);
        }

        public static DocumentObserver c4docobs_create(C4Database *db, string docID, DocumentObserverCallback callback, object context)
        {
            return new DocumentObserver(db, docID, callback, context);
        }
    }
}