//
// C4View.cs
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

using LiteCore.Util;
using C4SequenceNumber = System.UInt64;

namespace LiteCore.Interop
{
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
#if LITECORE_PACKAGED
    internal
#else
    public
#endif
         unsafe delegate void AccumulateDelegate(void* context, C4Key* key, C4Slice value);

#if LITECORE_PACKAGED
    internal
#else
    public
#endif
         unsafe delegate void ManagedAccumulateDelegate(object context, C4Key* key, C4Slice value);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
#if LITECORE_PACKAGED
    internal
#else
    public
#endif
         unsafe delegate C4Slice ReduceDelegate(void* context);

#if LITECORE_PACKAGED
    internal
#else
    public
#endif
         unsafe delegate string ManagedReduceDelegate(object context);

#if LITECORE_PACKAGED
    internal
#else
    public
#endif
         sealed class C4ManagedReduceFunction : IDisposable
    {
        private object _context;
        private readonly ManagedAccumulateDelegate _accumulate;

        private readonly ManagedReduceDelegate _reduce;

        private readonly Delegate[] _unmanaged = new Delegate[2];

        private C4String _lastReturn;

        public C4ReduceFunction Native { get; }

        public unsafe C4ManagedReduceFunction(ManagedAccumulateDelegate accumulate, ManagedReduceDelegate reduce, object context)
        {
            _accumulate = accumulate;
            _reduce = reduce;
            _context = context;
            _unmanaged[0] = new AccumulateDelegate(Accumulate);
            _unmanaged[1] = new ReduceDelegate(Reduce);
            Native = new C4ReduceFunction(_unmanaged[0] as AccumulateDelegate, 
                _unmanaged[1] as ReduceDelegate, null);
        }

        private unsafe void Accumulate(void* context, C4Key* key, C4Slice value)
        {
            _accumulate?.Invoke(_context, key, value);
        }

        private unsafe C4Slice Reduce(void* context)
        {
            if(_reduce == null) {
                return C4Slice.Null;
            }

            _lastReturn.Dispose();
            _lastReturn = new C4String(_reduce(_context));
            return _lastReturn.AsC4Slice();
        }

        public void Dispose()
        {
            _lastReturn.Dispose();
        }
    }

#if LITECORE_PACKAGED
    internal
#else
    public
#endif
    unsafe partial struct C4QueryOptions
    {
        public static readonly C4QueryOptions Default = new C4QueryOptions {
            limit = UInt64.MaxValue,
            inclusiveStart = true,
            inclusiveEnd = true,
            rankFullText = true
        };
    }

#if LITECORE_PACKAGED
    internal
#else
    public
#endif
    static unsafe partial class Native
    {
        public static bool c4indexer_emit(C4Indexer* indexer, C4Document* document, uint viewNumber, C4Key*[] emittedKeys, string[] emittedValues, C4Error* outError)
        {
            return c4indexer_emit(indexer, document, viewNumber, (uint)emittedKeys.Length, emittedKeys, emittedValues, outError);
        }
    }

#if LITECORE_PACKAGED
    internal
#else
    public
#endif
     static unsafe partial class NativeRaw
    {
        public static bool c4indexer_emit(C4Indexer* indexer, C4Document* document, uint viewNumber, C4Key*[] emittedKeys, C4Slice[] emittedValues, C4Error* outError)
        {
            fixed(C4Key** emittedKeys_ = emittedKeys) {
                return c4indexer_emit(indexer, document, viewNumber, (uint)emittedKeys.Length, emittedKeys_, emittedValues, outError);
            }
        }
    }
}
