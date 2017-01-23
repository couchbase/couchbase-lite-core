//
// View_native.cs
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
    public unsafe static partial class Native
    {
        public static C4View* c4view_open(C4Database* database, string path, string viewName, string version, C4DatabaseConfig* config, C4Error* outError)
        {
            using(var path_ = new C4String(path))
            using(var viewName_ = new C4String(viewName))
            using(var version_ = new C4String(version)) {
                return NativeRaw.c4view_open(database, path_.AsC4Slice(), viewName_.AsC4Slice(), version_.AsC4Slice(), config, outError);
            }
        }

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void c4view_free(C4View* view);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4view_close(C4View* view, C4Error* outError);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4view_rekey(C4View* view, C4EncryptionKey* newKey, C4Error* outError);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4view_eraseIndex(C4View* view, C4Error* outError);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4view_delete(C4View* view, C4Error* outError);

        public static bool c4view_deleteAtPath(string dbPath, C4DatabaseConfig* config, C4Error* outError)
        {
            using(var dbPath_ = new C4String(dbPath)) {
                return NativeRaw.c4view_deleteAtPath(dbPath_.AsC4Slice(), config, outError);
            }
        }

        public static bool c4view_deleteByName(C4Database* database, string viewName, C4Error* outError)
        {
            using(var viewName_ = new C4String(viewName)) {
                return NativeRaw.c4view_deleteByName(database, viewName_.AsC4Slice(), outError);
            }
        }

        public static void c4view_setMapVersion(C4View* view, string version)
        {
            using(var version_ = new C4String(version)) {
                NativeRaw.c4view_setMapVersion(view, version_.AsC4Slice());
            }
        }

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern ulong c4view_getTotalRows(C4View* view);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern ulong c4view_getLastSequenceIndexed(C4View* view);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern ulong c4view_getLastSequenceChangedAt(C4View* view);

        public static void c4view_setDocumentType(C4View* view, string docType)
        {
            using(var docType_ = new C4String(docType)) {
                NativeRaw.c4view_setDocumentType(view, docType_.AsC4Slice());
            }
        }

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void c4view_setOnCompactCallback(C4View* view, C4OnCompactCallback callback, void* context);

        public static C4Indexer* c4indexer_begin(C4Database* db, C4View*[] views, C4Error* outError)
        {
            fixed(C4View** views_ = views) {
                return NativeRaw.c4indexer_begin(db, views_, (UIntPtr)views.Length, outError);
            }
        }

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void c4indexer_triggerOnView(C4Indexer* indexer, C4View* view);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern C4DocEnumerator* c4indexer_enumerateDocuments(C4Indexer* indexer, C4Error* outError);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4indexer_shouldIndexDocument(C4Indexer* indexer, uint viewNumber, C4Document* doc);

        public static bool c4indexer_emit(C4Indexer* indexer, C4Document* document, uint viewNumber, uint emitCount, C4Key*[] emittedKeys, string[] emittedValues, C4Error* outError)
        {
            var c4Strings = new C4String[emittedValues.Length];
            for(int i = 0; i < emittedValues.Length; i++) {
                c4Strings[i] = new C4String(emittedValues[i]);
            }
            
            try {
                var c4Slices = c4Strings.Select(x => x.AsC4Slice()).ToArray();
                fixed(C4Key** emittedKeys_ = emittedKeys) {
                    return NativeRaw.c4indexer_emit(indexer, document, viewNumber, emitCount, emittedKeys_, c4Slices, outError);
                }
            } finally {
                foreach(var s in c4Strings) {
                    s.Dispose();
                }
            }
        }

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4indexer_emitList(C4Indexer* indexer, C4Document* doc, uint viewNumber, C4KeyValueList* kv, C4Error* outError);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4indexer_end(C4Indexer* indexer, [MarshalAs(UnmanagedType.U1)]bool commit, C4Error* outError);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern C4QueryEnumerator* c4view_query(C4View* view, C4QueryOptions* options, C4Error* outError);

        public static byte[] c4queryenum_customColumns(C4QueryEnumerator* e)
        {
            using(var retVal = NativeRaw.c4queryenum_customColumns(e)) {
                return ((C4Slice)retVal).ToArrayFast();
            }
        }

        public static string c4queryenum_fullTextMatched(C4QueryEnumerator* e, C4Error* outError)
        {
            using(var retVal = NativeRaw.c4queryenum_fullTextMatched(e, outError)) {
                return ((C4Slice)retVal).CreateString();
            }
        }

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4queryenum_next(C4QueryEnumerator* e, C4Error* outError);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void c4queryenum_close(C4QueryEnumerator* e);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void c4queryenum_free(C4QueryEnumerator* e);


    }
    
    public unsafe static partial class NativeRaw
    {
        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern C4View* c4view_open(C4Database* database, C4Slice path, C4Slice viewName, C4Slice version, C4DatabaseConfig* config, C4Error* outError);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4view_deleteAtPath(C4Slice dbPath, C4DatabaseConfig* config, C4Error* outError);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4view_deleteByName(C4Database* database, C4Slice viewName, C4Error* outError);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void c4view_setMapVersion(C4View* view, C4Slice version);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void c4view_setDocumentType(C4View* view, C4Slice docType);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern C4Indexer* c4indexer_begin(C4Database* db, C4View** views, UIntPtr viewCount, C4Error* outError);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4indexer_emit(C4Indexer* indexer, C4Document* document, uint viewNumber, uint emitCount, C4Key** emittedKeys, C4Slice[] emittedValues, C4Error* outError);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern C4SliceResult c4queryenum_customColumns(C4QueryEnumerator* e);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern C4SliceResult c4queryenum_fullTextMatched(C4QueryEnumerator* e, C4Error* outError);


    }
}
