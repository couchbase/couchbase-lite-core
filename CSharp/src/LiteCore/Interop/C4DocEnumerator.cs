//
// C4DocumentEnumerator.cs
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

using LiteCore.Util;
using C4SequenceNumber = System.UInt64;

namespace LiteCore.Interop
{
    [Flags]
    public enum C4EnumeratorFlags : ushort
    {
        Descending = 0x01,
        InclusiveStart = 0x02,
        InclusiveEnd = 0x04,
        IncludeDeleted = 0x08,
        IncudeNonConflicted = 0x10,
        IncludeBodies = 0x20
    }

    public struct C4EnumeratorOptions
    {
        public static readonly C4EnumeratorOptions Default = new C4EnumeratorOptions {
            skip = 0,
            flags = C4EnumeratorFlags.InclusiveStart | C4EnumeratorFlags.InclusiveEnd |
                                     C4EnumeratorFlags.IncudeNonConflicted | C4EnumeratorFlags.IncludeBodies
        };

        public ulong skip;
        public C4EnumeratorFlags flags;
    }

    public struct C4DocumentInfo
    {
        public C4DocumentFlags flags;
        public C4Slice docID;
        public C4Slice revID;
        public C4SequenceNumber sequence;
    }

    public struct C4DocEnumerator
    {
        
    }

    public unsafe static partial class Native
    {
        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void c4enum_close(C4DocEnumerator* e);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void c4enum_free(C4DocEnumerator* e);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern C4DocEnumerator* c4db_enumerateChanges(C4Database* database,
                                                                    C4SequenceNumber since,
                                                                    C4EnumeratorOptions* options,
                                                                    C4Error* outError);

        public static C4DocEnumerator* c4db_enumerateAllDocs(C4Database *database,
                                                             string startDocID,
                                                             string endDocID,
                                                             C4EnumeratorOptions *options,
                                                             C4Error *outError)
        {
            using(var startDocID_ = new C4String(startDocID))
            using(var endDocID_ = new C4String(endDocID)) {
                return NativeRaw.c4db_enumerateAllDocs(database, startDocID_.AsC4Slice(), endDocID_.AsC4Slice(),
                                                       options, outError);
            }
        }

        public static C4DocEnumerator* c4db_enumerateSomeDocs(C4Database *database,
                                                              string[] docIDs,
                                                              C4EnumeratorOptions *options,
                                                              C4Error *outError)
        {
            var c4Strings = new C4String[docIDs.Length];
            for(int i = 0; i < docIDs.Length; i++) {
                c4Strings[i] = new C4String(docIDs[i]);
            }

            try {
                var c4Slices = c4Strings.Select(x => x.AsC4Slice()).ToArray();
                return NativeRaw.c4db_enumerateSomeDocs(database, c4Slices, (UIntPtr)c4Slices.Length, options, outError);
            } finally {
                foreach(var s in c4Strings) {
                    s.Dispose();
                }
            }
        }

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4enum_next(C4DocEnumerator* e, C4Error* outError);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern C4Document* c4enum_getDocument(C4DocEnumerator* e, C4Error* outError);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4enum_getDocumentInfo(C4DocEnumerator* e, C4DocumentInfo* outInfo);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern C4Document* c4enum_nextDocument(C4DocEnumerator* e, C4Error* outError);
    }

    public unsafe static partial class NativeRaw
    {
        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern C4DocEnumerator* c4db_enumerateAllDocs(C4Database* database,
                                                                    C4Slice startDocID,
                                                                    C4Slice endDocID,
                                                                    C4EnumeratorOptions* options,
                                                                    C4Error* outError);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern C4DocEnumerator* c4db_enumerateSomeDocs(C4Database* database,
                                                                     C4Slice[] docIDs,
                                                                     UIntPtr docIDsCount,
                                                                     C4EnumeratorOptions* options,
                                                                     C4Error* outError);
    }
}
