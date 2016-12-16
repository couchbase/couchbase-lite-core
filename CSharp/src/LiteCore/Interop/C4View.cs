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
    public struct C4View
    {
        
    }

    public struct C4Indexer
    {
        
    }

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public unsafe delegate void AccumulateDelegate(void* context, C4Key* key, C4Slice value);

    public unsafe delegate void ManagedAccumulateDelegate(object context, C4Key* key, C4Slice value);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public unsafe delegate C4Slice ReduceDelegate(void* context);

    public unsafe delegate string ManagedReduceDelegate(object context);

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

    public sealed class C4ManagedReduceFunction : IDisposable
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

    public unsafe struct C4QueryOptions
    {
        public static readonly C4QueryOptions Default = new C4QueryOptions {
            limit = UInt64.MaxValue,
            inclusiveStart = true,
            inclusiveEnd = true,
            rankFullText = true
        };

        public ulong skip;
        public ulong limit;
        private byte _descending;
        private byte _inclusiveStart;
        private byte _inclusiveEnd;
        private byte _rankFullText;

        public C4Key* startKey;
        public C4Key* endKey;
        public C4Slice startKeyDocID;
        public C4Slice endKeyDocID;

        public C4Key** keys;
        private UIntPtr _keysCount;

        public C4ReduceFunction* reduce;
        public uint groupLevel;

        public bool descending 
        {
            get {
                return Convert.ToBoolean(_descending);
            }
            set {
                _descending = Convert.ToByte(value);
            }
        }

        public bool inclusiveStart
        {
            get {
                return Convert.ToBoolean(_inclusiveStart);
            }
            set {
                _inclusiveStart = Convert.ToByte(value);
            }
        }

        public bool inclusiveEnd
        {
            get {
                return Convert.ToBoolean(_inclusiveEnd);
            }
            set {
                _inclusiveEnd = Convert.ToByte(value);
            }
        }

        public bool rankFullText
        {
            get {
                return Convert.ToBoolean(_rankFullText);
            }
            set {
                _rankFullText = Convert.ToByte(value);
            }
        }

        public ulong keysCount
        {
            get {
                return _keysCount.ToUInt64();
            }
            set {
                _keysCount = (UIntPtr)value;
            }
        }
    }

    public struct C4FullTextTerm
    {
        public uint termIndex;
        public uint start, length;
    }

    public unsafe struct C4QueryEnumerator
    {
        // All query types:
        public C4Slice docID;
        public C4SequenceNumber docSequence;

        // Map/reduce only
        public C4KeyReader key;
        public C4Slice value;

        // Expression-based only:
        public C4Slice revID;
        public C4DocumentFlags docFlags;

        // Full-text only:
        public uint fullTextTermCount;
        public C4FullTextTerm* fullTextTerms;
    }
    
    public static unsafe partial class Native
    {
        public static bool c4indexer_emit(C4Indexer* indexer, C4Document* document, uint viewNumber, C4Key*[] emittedKeys, string[] emittedValues, C4Error* outError)
        {
            return c4indexer_emit(indexer, document, viewNumber, (uint)emittedKeys.Length, emittedKeys, emittedValues, outError);
        }
    }
    
    public static unsafe partial class NativeRaw
    {
        public static bool c4indexer_emit(C4Indexer* indexer, C4Document* document, uint viewNumber, C4Key*[] emittedKeys, C4Slice[] emittedValues, C4Error* outError)
        {
            fixed(C4Key** emittedKeys_ = emittedKeys) {
                return c4indexer_emit(indexer, document, viewNumber, (uint)emittedKeys.Length, emittedKeys_, emittedValues, outError);
            }
        }
    }
}
