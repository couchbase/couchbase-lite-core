//
// C4DBQuery.cs
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
    public struct C4Query
    {
        
    }

    public static unsafe partial class Native
    {
        public static C4Query* c4query_new(C4Database* db, string queryExpression, string sortExpression, C4Error *outError)
        {
            using(var queryExpression_ = new C4String(queryExpression))
            using(var sortExpression_ = new C4String(sortExpression)) {
                return NativeRaw.c4query_new(db, queryExpression_.AsC4Slice(), sortExpression_.AsC4Slice(), outError);
            }
        }
        
        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void c4query_free(C4Query* query);

        public static C4QueryEnumerator* c4query_run(C4Query* query,
                                                     C4QueryOptions* options,
                                                     byte[] encodedParams,
                                                     C4Error* outError)
        {
            fixed(byte *b = encodedParams) {
                var length = encodedParams == null ? 0UL : (ulong)encodedParams.Length;
                return NativeRaw.c4query_run(query, options, new C4Slice(b, length), outError);
            }
        }

        public static bool c4db_createIndex(C4Database* db, string expression, C4Error* outError)
        {
            using(var expression_ = new C4String(expression)) {
                return NativeRaw.c4db_createIndex(db, expression_.AsC4Slice(), outError);
            }
        }

        public static bool c4db_deleteIndex(C4Database* db, string expression, C4Error* outError)
        {
            using(var expression_ = new C4String(expression)) {
                return NativeRaw.c4db_deleteIndex(db, expression_.AsC4Slice(), outError);
            }
        }
    }

    public static unsafe partial class NativeRaw
    {
        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern C4Query* c4query_new(C4Database* db, C4Slice queryExpression, C4Slice sortExpression, C4Error* outError);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern C4QueryEnumerator* c4query_run(C4Query* query,
                                                            C4QueryOptions* options,
                                                            C4Slice encodedParams,
                                                            C4Error* outError);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4db_createIndex(C4Database* db, C4Slice expression, C4Error* outError);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4db_deleteIndex(C4Database* db, C4Slice expression, C4Error* outError);
    }
}
