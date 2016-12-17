//
// Fleece.cs
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
using System.Collections.Concurrent;
using System.Runtime.InteropServices;
using System.Text;

using LiteCore.Util;

namespace LiteCore.Interop
{
    public struct FLValue
    {
        
    }

    public struct FLArray
    {
        
    }

    public struct FLDict
    {
        
    }

    public struct FLEncoder
    {
        
    }

    public struct FLSharedKeys
    {

    }

    public struct FLKeyPath
    {
        
    }

    public enum FLValueType
    {
        Undefined = -1,
        Null,
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
        InternalError
    }

    public unsafe struct FLSlice
    {
        public static readonly FLSlice Null = new FLSlice(null, 0);
        public void* buf;
        private UIntPtr _size;

        private static readonly ConcurrentDictionary<string, FLSlice> _Constants =
            new ConcurrentDictionary<string, FLSlice>();

        public ulong size
        {
            get {
                return _size.ToUInt64();
            }
            set {
                _size = (UIntPtr)value;
            }
        }

        public FLSlice(void* buf, ulong size)
        {
            this.buf = buf;
            this._size = (UIntPtr)size;
        }

        public static FLSlice Constant(string input)
        {
            // Warning: This creates unmanaged memory that is intended never to be freed
            // You should only use it with constant strings
            return _Constants.GetOrAdd(input, key => {
                return Allocate(key);
            });
        }

        public static FLSlice Allocate(string input)
        {
            var bytes = Encoding.UTF8.GetBytes(input);
            var intPtr = Marshal.AllocHGlobal(bytes.Length);
            Marshal.Copy(bytes, 0, intPtr, bytes.Length);
            return new FLSlice(intPtr.ToPointer(), (ulong)bytes.Length);
        }

        public static void Free(FLSlice slice)
        {
            Marshal.FreeHGlobal(new IntPtr(slice.buf));
            slice.buf = null;
            slice.size = 0;
        }

        public string CreateString()
        {
            if(buf == null) {
                return null;
            }


            var tmp = new IntPtr(buf);
            var bytes = new byte[size];
            Marshal.Copy(tmp, bytes, 0, bytes.Length);
            return Encoding.UTF8.GetString(bytes, 0, bytes.Length);
        }

        public override int GetHashCode()
        {
            unchecked {
                int hash = 17;

                hash = hash * 23 + (int)size;
                var ptr = (byte*)buf;
                if(ptr != null) {
                    hash = hash * 23 + ptr[size - 1];
                }

                return hash;
            }
        }

        public override bool Equals(object obj)
        {
            if(!(obj is FLSlice)) {
                return false;
            }

            return Native.FLSlice_Compare(this, (FLSlice)obj) == 0;
        }
    }

    public unsafe struct FLSliceResult : IDisposable
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

        public static implicit operator FLSlice(FLSliceResult input)
        {
            return new FLSlice(input.buf, input.size);
        }

        public static implicit operator C4Slice(FLSliceResult input)
        {
            return new C4Slice(input.buf, input.size);
        }

        public void Dispose()
        {
            Native.FLSliceResult_Free(this);
        }
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

    public unsafe struct FLDictIterator
    {
        #pragma warning disable CS0169

        private void* _private1;
        private uint _private2;
        private byte _private3;
        private void* _private4;
        private void* _private5;

        #pragma warning restore CS0169
    }

    public unsafe struct FLDictKey
    {
        #pragma warning disable CS0169

        //HACK: Cannot have an inline array of pointers
        private void* _private1a;
        private void* _private1b;
        private void* _private1c;
        private uint _private2;
        private byte _private3;

        #pragma warning restore CS0169
    }

    public static unsafe partial class Native
    {
        public static byte[] FLData_ConvertJSON(byte[] json, FLError* outError)
        {
            fixed(byte* b = json) {
                var slice = new FLSlice(b, (ulong)json.Length);
                using(var retVal = NativeRaw.FLData_ConvertJSON(slice, outError)) {
                    FLSlice r = retVal;
                    return ((C4Slice)r).ToArrayFast();
                }
            }
        }
    }
}
