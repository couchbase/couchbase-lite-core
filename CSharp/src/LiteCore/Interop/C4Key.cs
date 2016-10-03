//
// C4Key.cs
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

namespace LiteCore.Interop
{
    public struct C4GeoArea
    {
        public double xmin, ymin, xmax, ymax;
    }

    public struct C4Key
    {
        
    }

    public unsafe struct C4KeyReader
    {
        public void* bytes;
        private UIntPtr _length;

        public ulong length
        {
            get {
                return _length.ToUInt64();
            }
            set {
                _length = (UIntPtr)value;
            }
        }
    }

    public enum C4KeyToken : byte
    {
        Null,
        Bool,
        Number,
        String,
        Array,
        Map,
        EndSequence,
        Special,
        Error = 255
    }

    public struct C4KeyValueList
    {
        
    }

    public unsafe static partial class Native
    {
        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern C4Key* c4key_new();

        public static C4Key* c4key_withBytes(byte[] bytes)
        {
            fixed(void* b = bytes) {
                var c4 = new C4Slice(b, (ulong)bytes.Length);
                return NativeRaw.c4key_withBytes(c4);
            }
        }

        public static C4Key* c4key_newFullTextString(string text, string language)
        {
            using(var text_ = new C4String(text))
            using(var language_ = new C4String(language)) {
                return NativeRaw.c4key_newFullTextString(text_.AsC4Slice(), language_.AsC4Slice());
            }
        }

        public static C4Key* c4key_newGeoJSON(string geoJSON, C4GeoArea boundingArea)
        {
            using(var geoJSON_ = new C4String(geoJSON)) {
                return NativeRaw.c4key_newGeoJSON(geoJSON_.AsC4Slice(), boundingArea);
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
        public static extern void c4key_addNumber(C4Key* key, double num);

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

        public static bool c4key_setDefaultFullTextLanguage(string languageName, bool stripDiacriticals)
        {
            using(var languageName_ = new C4String(languageName)) {
                return NativeRaw.c4key_setDefaultFullTextLanguage(languageName_.AsC4Slice(), stripDiacriticals);
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
                return retVal.CreateString();
            }
        }

        public static string c4key_toJSON(C4KeyReader* reader)
        {
            using(var retVal = NativeRaw.c4key_toJSON(reader)) {
                return retVal.CreateString();
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

    public unsafe static partial class NativeRaw
    {
        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern C4Key* c4key_withBytes(C4Slice slice);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern C4Key* c4key_newFullTextString(C4Slice text, C4Slice language);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern C4Key* c4key_newGeoJSON(C4Slice geoJSON, C4GeoArea boundingArea);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void c4key_addString(C4Key* key, C4Slice str);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void c4key_addMapKey(C4Key* key, C4Slice str);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4key_setDefaultFullTextLanguage(C4Slice languageName, [MarshalAs(UnmanagedType.U1)]bool stripDiacriticals);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern C4Slice c4key_readString(C4KeyReader* reader);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern C4Slice c4key_toJSON(C4KeyReader* reader);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void c4kv_add(C4KeyValueList* kv, C4Key* key, C4Slice value);
    }
}
