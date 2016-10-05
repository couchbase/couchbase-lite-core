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
                var bytes = Encoding.UTF8.GetBytes(key);
                var intPtr = Marshal.AllocHGlobal(bytes.Length);
                Marshal.Copy(bytes, 0, intPtr, bytes.Length);
                return new FLSlice(intPtr.ToPointer(), (ulong)bytes.Length);
            });
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
        private void* _private1;
        private uint _private2;
        private byte _private3;
        private void* _private4;
    }

    public unsafe struct FLDictIterator
    {
        private void* _private1;
        private uint _private2;
        private byte _private3;
        private void* _private4;
        private void* _private5;
    }

    public unsafe struct FLDictKey
    {
        //HACK: Cannot have an inline array of pointers
        private void* _private1a;
        private void* _private1b;
        private void* _private1c;
        private uint _private2;
        private byte _private3;
    }

    public static unsafe partial class Native
    {
        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void FLSliceResult_Free(FLSliceResult slice);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int FLSlice_Compare(FLSlice left, FLSlice right);

        public static FLValue* FLValue_FromData(byte[] data)
        {
            fixed(byte* b = data) {
                return NativeRaw.FLValue_FromData(new FLSlice(b, (ulong)data.Length));
            }
        }

        public static FLValue* FLValue_FromTrustedData(byte[] data)
        {
            fixed (byte* b = data) {
                return NativeRaw.FLValue_FromTrustedData(new FLSlice(b, (ulong)data.Length));
            }
        }

        public static string FLData_ConvertJSON(string json, FLError* outError)
        {
            using(var json_ = new C4String(json)) {
                using(var retVal = NativeRaw.FLData_ConvertJSON((FLSlice)json_.AsC4Slice(), outError)) {
                    return ((FLSlice)retVal).CreateString();
                }
            }
        }

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

        public static string FLData_Dump(FLSlice data)
        {
            using(var retVal = NativeRaw.FLData_Dump(data)) {
                return ((FLSlice)retVal).CreateString();
            }
        }

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern FLValueType FLValue_GetType(FLValue* value);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool FLValue_IsInteger(FLValue* value);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool FLValue_IsUnsigned(FLValue* value);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool FLValue_IsDouble(FLValue* value);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool FLValue_AsBool(FLValue* value);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern long FLValue_AsInt(FLValue* value);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern ulong FLValue_AsUnsigned(FLValue* value);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern float FLValue_AsFloat(FLValue* value);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern double FLValue_AsDouble(FLValue* value);

        public static string FLValue_AsString(FLValue* value)
        {
            return NativeRaw.FLValue_AsString(value).CreateString();
        }

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern FLArray* FLValue_AsArray(FLValue* value);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern FLDict* FLValue_AsDict(FLValue* value);

        public static string FLValue_ToString(FLValue* value)
        {
            using(var retVal = NativeRaw.FLValue_ToString(value)) {
                return ((FLSlice)retVal).CreateString();
            }
        }

        public static string FLValue_ToJSON(FLValue* value)
        {
            using(var retVal = NativeRaw.FLValue_ToJSON(value)) {
                return ((FLSlice)retVal).CreateString();
            }
        }

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern uint FLArray_Count(FLArray* array);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern FLValue* FLArray_Get(FLArray* array, uint index);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void FLArrayIterator_Begin(FLArray* array, FLArrayIterator* iterator);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern FLValue* FLArrayIterator_GetValue(FLArrayIterator* iterator);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool FLArrayIterator_Next(FLArrayIterator* iterator);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern uint FLDict_Count(FLDict* dict);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern FLValue* FLDict_Get(FLDict* dict, FLSlice key);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern FLValue* FLDict_GetUnsorted(FLDict* dict, FLSlice key);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void FLDictIterator_Begin(FLDict* dict, FLDictIterator* iterator);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern FLValue* FLDictIterator_GetKey(FLDictIterator* iterator);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern FLValue* FLDictIterator_GetValue(FLDictIterator* iterator);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool FLDictIterator_Next(FLDictIterator* iterator);

        public static FLDictKey FLDictKey_Init(string str, bool cachePointers)
        {
            using(var str_ = new C4String(str)) {
                return NativeRaw.FLDictKey_Init((FLSlice)str_.AsC4Slice(), cachePointers);
            }
        }

        public static string FLDictKey_GetString(FLDictKey* key)
        {
            return NativeRaw.FLDictKey_GetString(key).CreateString();
        }

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern FLValue* FLDict_GetWithKey(FLDict* dict, FLDictKey* key);

        public static ulong FLDict_GetWithKeys(FLDict* dict, FLDictKey[] keys, FLValue[] values)
        {
            var count = (UIntPtr)keys.Length;
            return NativeRaw.FLDict_GetWithKeys(dict, keys, values, count).ToUInt64();
        }

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern FLEncoder* FLEncoder_New();

        public static FLEncoder* FLEncoder_NewWithOptions(ulong reserveSize, bool uniqueStrings, bool sortKeys)
        {
            var rs = (UIntPtr)reserveSize;
            return NativeRaw.FLEncoder_NewWithOptions(rs, uniqueStrings, sortKeys);
        }

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void FLEncoder_Free(FLEncoder* encoder);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void FLEncoder_Reset(FLEncoder* encoder);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool FLEncoder_WriteNull(FLEncoder* encoder);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool FLEncoder_WriteBool(FLEncoder* encoder, [MarshalAs(UnmanagedType.U1)]bool val);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool FLEncoder_WriteInt(FLEncoder* encoder, long val);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool FLEncoder_WriteUInt(FLEncoder* encoder, ulong val);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool FLEncoder_WriteFloat(FLEncoder* encoder, float val);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool FLEncoder_WriteDouble(FLEncoder* encoder, double val);

        public static bool FLEncoder_WriteString(FLEncoder* encoder, string val)
        {
            using(var val_ = new C4String(val)) {
                return NativeRaw.FLEncoder_WriteString(encoder, (FLSlice)val_.AsC4Slice());
            }
        }

        public static bool FLEncoder_WriteData(FLEncoder* encoder, byte[] val)
        {
            fixed(byte* b = val) {
                return NativeRaw.FLEncoder_WriteData(encoder, new FLSlice(b, (ulong)val.Length));
            }
        }

        public static bool FLEncoder_BeginArray(FLEncoder* encoder, ulong reserveSize)
        {
            return NativeRaw.FLEncoder_BeginArray(encoder, (UIntPtr)reserveSize);
        }

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool FLEncoder_EndArray(FLEncoder* encoder);

        public static bool FLEncoder_BeginDict(FLEncoder* encoder, ulong reserveSize)
        {
            return NativeRaw.FLEncoder_BeginDict(encoder, (UIntPtr)reserveSize);
        }

        public static bool FLEncoder_WriteKey(FLEncoder* encoder, string key)
        {
            using(var key_ = new C4String(key)) {
                return NativeRaw.FLEncoder_WriteKey(encoder, (FLSlice)key_.AsC4Slice());
            }
        }

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool FLEncoder_EndDict(FLEncoder* encoder);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool FLEncoder_WriteValue(FLEncoder* encoder, FLValue* value);

        public static bool FLEncoder_ConvertJSON(FLEncoder* encoder, byte[] json)
        {
            fixed(byte* b = json) {
                return NativeRaw.FLEncoder_ConvertJSON(encoder, new FLSlice(b, (ulong)json.Length));
            }
        }

        public static byte[] FLEncoder_Finish(FLEncoder* encoder, FLError *err)
        {
            using(var retVal = NativeRaw.FLEncoder_Finish(encoder, err)) {
                return ((C4Slice)retVal).ToArrayFast();
            }
        }

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern FLError FLEncoder_GetError(FLEncoder* encoder);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.LPStr)]
        public static extern string FLEncoder_GetErrorMessage(FLEncoder* encoder);
    }

    public static unsafe partial class NativeRaw
    {
        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern FLValue* FLValue_FromData(FLSlice data);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern FLValue* FLValue_FromTrustedData(FLSlice data);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern FLSliceResult FLData_ConvertJSON(FLSlice json, FLError* outError);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern FLSliceResult FLData_Dump(FLSlice data);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern FLSlice FLValue_AsString(FLValue* value);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern FLSliceResult FLValue_ToString(FLValue* value);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern FLSliceResult FLValue_ToJSON(FLValue* value);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern FLDictKey FLDictKey_Init(FLSlice str, [MarshalAs(UnmanagedType.U1)]bool cachePointers);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern FLSlice FLDictKey_GetString(FLDictKey* key);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern UIntPtr FLDict_GetWithKeys(FLDict* dict, [Out]FLDictKey[] keys, [Out]FLValue[] values, UIntPtr count);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern FLEncoder* FLEncoder_NewWithOptions(UIntPtr reserveSize,
                                                                [MarshalAs(UnmanagedType.U1)]bool uniqueStrings,
                                                                [MarshalAs(UnmanagedType.U1)]bool sortKeys);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool FLEncoder_WriteString(FLEncoder* encoder, FLSlice val);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool FLEncoder_WriteData(FLEncoder* encoder, FLSlice val);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool FLEncoder_BeginArray(FLEncoder* encoder, UIntPtr reserveSize);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool FLEncoder_BeginDict(FLEncoder* encoder, UIntPtr reserveSize);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool FLEncoder_WriteKey(FLEncoder* encoder, FLSlice key);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool FLEncoder_ConvertJSON(FLEncoder* encoder, FLSlice json);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern FLSliceResult FLEncoder_Finish(FLEncoder* encoder, FLError* err);


    }
}
