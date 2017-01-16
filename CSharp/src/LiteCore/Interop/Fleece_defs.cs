//
// Fleece_defs.cs
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
    public enum FLValueType
    {
        Undefined = -1,
        Null = 0,
        Boolean,
        Number,
        String,
        Data,
        Array,
        Dict
    }

    public enum FLError
    {
        NoError = 0,
        MemoryError,
        OutOfRange,
        InvalidData,
        EncodeError,
        JSONError,
        UnknownValue,
        InternalError,
        NotFound
    }

		public unsafe struct FLDictIterator
    {
        #pragma warning disable CS0169

        private void* _private1;
        private uint _private2;
        private byte _private3;

        // _private4[3]
        private void* _private4;
        private void* _private5;
        private void* _private6;

        #pragma warning restore CS0169
    }

    public unsafe struct FLEncoder
    {
    }

    public unsafe struct FLDictKey
    {
        #pragma warning disable CS0169

        // _private1[3] 
        private void* _private1a;
        private void* _private1b;
        private void* _private1c;
        private uint _private2;
        private byte _private3;

        #pragma warning restore CS0169
    }

    public unsafe struct FLArray
    {
    }

    public unsafe struct FLArrayIterator
    {
        #pragma warning disable CS0169

        private void* _private1;
        private uint _private2;
        private byte _private3;
        private void* _private4;

        #pragma warning restore CS0169
    }

    public unsafe partial struct FLSliceResult
    {
        public void* buf;
        private UIntPtr _size;

        public ulong size
        {
            get {
                return _size.ToUInt64();
            }
            set {
                _size = (UIntPtr)value;
            }
        }
    }

    public unsafe struct FLValue
    {
    }

    public unsafe struct FLKeyPath
    {
    }

    public unsafe struct FLSharedKeys
    {
    }

    public unsafe partial struct FLSlice
    {
        public void* buf;
        private UIntPtr _size;

        public ulong size
        {
            get {
                return _size.ToUInt64();
            }
            set {
                _size = (UIntPtr)value;
            }
        }
    }

    public unsafe struct FLDict
    {
    }
}