//
// Key_native.cs
//
// Author:
// 	Jim Borden  <jim.borden@couchbase.com>
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
using System.Linq;
using System.Runtime.InteropServices;

using LiteCore.Util;

namespace LiteCore.Interop
{
#if LITECORE_PACKAGED
    internal
#else
    public
#endif 
    unsafe static partial class Native
    {
        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern C4Key* c4key_new();

        public static C4Key* c4key_withBytes(byte[] slice)
        {
            fixed(byte *slice_ = slice) {
                return NativeRaw.c4key_withBytes(new C4Slice(slice_, (ulong)slice.Length));
            }
        }

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void c4key_reset(C4Key* key);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void c4key_free(C4Key* key);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void c4key_addNull(C4Key* key);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void c4key_addBool(C4Key* key, [MarshalAs(UnmanagedType.U1)]bool b);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void c4key_addNumber(C4Key* key, double d);

        public static void c4key_addString(C4Key* key, string str)
        {
            using(var str_ = new C4String(str)) {
                NativeRaw.c4key_addString(key, str_.AsC4Slice());
            }
        }

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void c4key_beginArray(C4Key* key);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void c4key_endArray(C4Key* key);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void c4key_beginMap(C4Key* key);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void c4key_endMap(C4Key* key);

        public static void c4key_addMapKey(C4Key* key, string str)
        {
            using(var str_ = new C4String(str)) {
                NativeRaw.c4key_addMapKey(key, str_.AsC4Slice());
            }
        }

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern C4KeyReader c4key_read(C4Key* key);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern C4KeyReader* c4key_newReader(C4Key* key);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void c4key_freeReader(C4KeyReader* reader);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern C4KeyToken c4key_peek(C4KeyReader* reader);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void c4key_skipToken(C4KeyReader* reader);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4key_readBool(C4KeyReader* reader);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern double c4key_readNumber(C4KeyReader* reader);

        public static string c4key_readString(C4KeyReader* reader)
        {
            using(var retVal = NativeRaw.c4key_readString(reader)) {
                return ((C4Slice)retVal).CreateString();
            }
        }

        public static string c4key_toJSON(C4KeyReader* reader)
        {
            using(var retVal = NativeRaw.c4key_toJSON(reader)) {
                return ((C4Slice)retVal).CreateString();
            }
        }

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern C4KeyValueList* c4kv_new();

        public static void c4kv_add(C4KeyValueList* kv, C4Key* key, string value)
        {
            using(var value_ = new C4String(value)) {
                NativeRaw.c4kv_add(kv, key, value_.AsC4Slice());
            }
        }

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void c4kv_reset(C4KeyValueList* kv);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void c4kv_free(C4KeyValueList* kv);


    }
    
#if LITECORE_PACKAGED
    internal
#else
    public
#endif 
    unsafe static partial class NativeRaw
    {
        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern C4Key* c4key_withBytes(C4Slice slice);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void c4key_addString(C4Key* key, C4Slice str);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void c4key_addMapKey(C4Key* key, C4Slice str);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern C4SliceResult c4key_readString(C4KeyReader* reader);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern C4SliceResult c4key_toJSON(C4KeyReader* reader);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void c4kv_add(C4KeyValueList* kv, C4Key* key, C4Slice value);


    }
}
