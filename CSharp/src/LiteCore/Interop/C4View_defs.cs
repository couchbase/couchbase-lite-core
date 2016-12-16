//
// C4View_defs.cs
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
using System.Linq;
using System.Runtime.InteropServices;
using System.Threading;

using LiteCore.Util;

namespace LiteCore.Interop
{


    public unsafe struct C4View
    {
    }

    public unsafe partial struct C4QueryOptions
    {
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

    public unsafe struct C4QueryEnumerator
    {
        public C4Slice docID;
        public ulong docSequence;
        public C4KeyReader key;
        public C4Slice value;
        public C4Slice revID;
        public C4DocumentFlags docFlags;
        public uint fullTextTermCount;
        public C4FullTextTerm* fullTextTerms;
    }

    public unsafe struct C4Indexer
    {
    }

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

    public struct C4FullTextTerm
    {
        public uint termIndex;
        public uint start, length;
    }
}