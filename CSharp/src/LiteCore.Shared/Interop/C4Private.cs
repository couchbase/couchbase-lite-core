//
// C4Private.cs
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

using System.Diagnostics.CodeAnalysis;
using System.Runtime.InteropServices;

using LiteCore.Util;

namespace LiteCore.Interop
{
#if LITECORE_PACKAGED
    internal
#else
    public
#endif
         static unsafe class NativePrivate
    {
        [DllImport(Constants.DllName, CallingConvention=CallingConvention.Cdecl)]
        public static extern void c4log_warnOnErrors(bool warn);
    }

#if LITECORE_PACKAGED
    internal
#else
    public
#endif
         static unsafe class NativeRawPrivate
    {
        [DllImport(Constants.DllName, CallingConvention=CallingConvention.Cdecl)]
        public static extern C4Document* c4doc_getForPut(C4Database* database, C4Slice docID, C4Slice parentRevID, [MarshalAs(UnmanagedType.U1)]bool deleting, 
            [MarshalAs(UnmanagedType.U1)]bool allowConflict, C4Error* outError);
    }
}