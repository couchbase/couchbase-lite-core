//
// C4ExpiryEnumerator.cs
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
    public struct C4ExpiryEnumerator
    {
        
    }

    public unsafe static partial class Native
    {
        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern C4ExpiryEnumerator* c4db_enumerateExpired(C4Database* database, C4Error* outError);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4exp_next(C4ExpiryEnumerator* e);

        public static string c4exp_getDocID(C4ExpiryEnumerator* e)
        {
            using(var retVal = NativeRaw.c4exp_getDocID(e)) {
                return retVal.CreateString();
            }
        }

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4exp_purgeExpired(C4ExpiryEnumerator* e, C4Error* outError);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void c4exp_close(C4ExpiryEnumerator* e);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void c4exp_free(C4ExpiryEnumerator* e);
    }

    public unsafe static partial class NativeRaw
    {
        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern C4Slice c4exp_getDocID(C4ExpiryEnumerator* e);
    }
}
