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

        public C4DatabaseObserver* Observer { get; private set; }

        public DatabaseObserver(C4Database* database, DatabaseObserverCallback callback, object context)
        {
            _context = context;
            _callback = callback;
            Observer = (C4DatabaseObserver *)LiteCoreBridge.Check(err => Native.c4dbobs_create(database, DBObserverCallback, null));
        }

        private void DBObserverCallback(C4DatabaseObserver* observer, void* context)
        {
            _callback?.Invoke(observer, _context);
        }

        public void Dispose()
        {
            Native.c4dbobs_free(Observer);
            Observer = null;
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

        public C4DocumentObserver* Observer { get; private set; }

        public DocumentObserver(C4Database* database, string docID, DocumentObserverCallback callback, object context)
        {
            _context = context;
            _callback = callback;
            Observer = (C4DocumentObserver *)LiteCoreBridge.Check(err => Native.c4docobs_create(database, docID, DocObserverCallback, null));
        }

        private void DocObserverCallback(C4DocumentObserver* observer, C4Slice docID, ulong sequence, void* context)
        {
            _callback?.Invoke(observer, docID.CreateString(), sequence, _context);
        }

        public void Dispose()
        {
            Native.c4docobs_free(Observer);
            Observer = null;
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