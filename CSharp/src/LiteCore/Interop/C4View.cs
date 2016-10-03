//
// C4View.cs
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
using C4SequenceNumber = System.Int64;

namespace LiteCore.Interop
{
    public struct C4View
    {
        
    }

    public struct C4Indexer
    {
        
    }

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public unsafe delegate void AccumulateDelegate(void* context, C4Key* key, C4Slice value);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public unsafe delegate C4Slice ReduceDelegate(void* context);

    public unsafe struct C4ReduceFunction
    {
        public IntPtr accumulate;
        public IntPtr reduce;
        public void* context;

        public C4ReduceFunction(AccumulateDelegate accumulate, ReduceDelegate reduce, void* context)
        {
            this.accumulate = Marshal.GetFunctionPointerForDelegate(accumulate);
            this.reduce = Marshal.GetFunctionPointerForDelegate(reduce);
            this.context = context;
        }
    }

    public unsafe struct C4QueryOptions
    {
        public static readonly C4QueryOptions Default;

        public ulong skip;
        public ulong limit;
        private byte _descending;
        private byte _inclusiveStart;
        private byte _inclusiveEnd;
        private byte _rankFullText;

        public C4Key* startKey;
        public C4Key* endKey;
        public C4Slice startKeyDocID;
        public C4Slice endKeyDocID;

        public C4Key** keys;
        private UIntPtr _keysCount;

        public C4ReduceFunction reduce;
        public uint groupLevel;

        public bool descending 
        {
            get {
                return Convert.ToBoolean(_descending);
            }
            set {
                _descending = Convert.ToByte(value);
            }
        }

        public bool inclusiveStart
        {
            get {
                return Convert.ToBoolean(_inclusiveStart);
            }
            set {
                _inclusiveStart = Convert.ToByte(value);
            }
        }

        public bool inclusiveEnd
        {
            get {
                return Convert.ToBoolean(_inclusiveEnd);
            }
            set {
                _inclusiveEnd = Convert.ToByte(value);
            }
        }

        public bool rankFullText
        {
            get {
                return Convert.ToBoolean(_rankFullText);
            }
            set {
                _rankFullText = Convert.ToByte(value);
            }
        }

        public ulong keysCount
        {
            get {
                return _keysCount.ToUInt64();
            }
            set {
                _keysCount = (UIntPtr)value;
            }
        }
    }

    public struct C4FullTextTerm
    {
        public uint termIndex;
        public uint start, length;
    }

    public unsafe struct C4QueryEnumerator
    {
        public C4Slice docID;
        public C4SequenceNumber docSequence;
        public C4Slice value;

        public C4KeyReader key;

        public uint fullTextID;
        public uint fullTextTermCount;
        public C4FullTextTerm* fullTextTerms;

        public C4GeoArea geoBBox;
        public C4Slice geoJSON;
    }

    public unsafe static partial class Native
    {
        public static C4View* c4view_open(C4Database* database,
                                          string path,
                                          string viewName,
                                          string version,
                                          C4DatabaseConfig* config,
                                          C4Error* outError)
        {
            using(var path_ = new C4String(path))
            using(var viewName_ = new C4String(viewName))
            using(var version_ = new C4String(version)) {
                return NativeRaw.c4view_open(database, path_.AsC4Slice(), viewName_.AsC4Slice(), version_.AsC4Slice(),
                                             config, outError);
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

        public static bool c4view_deleteAtPath(string path, C4DatabaseConfig* config, C4Error* outError)
        {
            using(var path_ = new C4String(path)) {
                return NativeRaw.c4view_deleteAtPath(path_.AsC4Slice(), config, outError);
            }
        }

        public static bool c4view_deleteByName(C4Database* database, string name, C4Error* outError)
        {
            using(var name_ = new C4String(name)) {
                return NativeRaw.c4view_deleteByName(database, name_.AsC4Slice(), outError);
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
        public static extern C4SequenceNumber c4view_getLastSequenceIndexed(C4View* view);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern C4SequenceNumber c4view_getLastSequenceChangedAt(C4View* view);

        public static void c4view_setDocumentType(C4View* view, string docType)
        {
            using(var docType_ = new C4String(docType)) {
                NativeRaw.c4view_setDocumentType(view, docType_.AsC4Slice());
            }
        }

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void c4view_setOnCompactCallback(C4View* view, C4OnCompactCallback cb, void* context);

        public static C4Indexer* c4indexer_begin(C4Database *db, C4View*[] views, C4Error *outError)
        {
            fixed(C4View** p = views) {
                return NativeRaw.c4indexer_begin(db, p, (UIntPtr)views.Length, outError);
            }
        }

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void c4view_triggerOnView(C4Indexer* indexer, C4View* view);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern C4DocEnumerator* c4indexer_enumerateDocuments(C4Indexer* indexer, C4Error* outError);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4view_shouldIndexDocument(C4Indexer* indexer, uint viewNumber, C4Document* doc);

        public static bool c4indexer_emit(C4Indexer* indexer,
                                          C4Document* document,
                                          uint viewNumber,
                                          C4Key*[] emittedKeys,
                                          C4Slice[] emittedValues,
                                          C4Error* outError)
        {
            fixed(C4Key** p = emittedKeys) {
                return NativeRaw.c4indexer_emit(indexer, document, viewNumber, (uint)emittedKeys.Length, p,
                                                emittedValues, outError);
            }
        }

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4indexer_emitList(C4Indexer* indexer,
                                                     C4Document* doc,
                                                     uint viewNumber,
                                                     C4KeyValueList* kv,
                                                     C4Error* outError);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4indexer_end(C4Indexer* indexer, [MarshalAs(UnmanagedType.U1)]bool commit, C4Error* outError);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern C4QueryEnumerator* c4view_query(C4View* view, C4QueryOptions* options, C4Error* outError);

        public static C4QueryEnumerator* c4view_fullTextQuery(C4View* view,
                                                              string queryString,
                                                              string queryStringLanguage,
                                                              C4QueryOptions* c4Options,
                                                              C4Error* outError)
        {
            using(var queryString_ = new C4String(queryString))
            using(var queryStringLanguage_ = new C4String(queryStringLanguage)) {
                return NativeRaw.c4view_fullTextQuery(view, queryString_.AsC4Slice(), queryStringLanguage_.AsC4Slice(),
                                                      c4Options, outError);
            }
        }

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern C4QueryEnumerator* c4view_geoQuery(C4View* view, C4GeoArea area, C4Error* outError);

        public static string c4queryenum_fullTextMatched(C4QueryEnumerator* e)
        {
            using(var retVal = NativeRaw.c4queryenum_fullTextMatched(e)) {
                return retVal.CreateString();
            }
        }

        public static string c4view_fullTextMatched(C4View* view,
                                             string docID,
                                             C4SequenceNumber seq,
                                             uint fullTextID,
                                             C4Error* outError)
        {
            using(var docID_ = new C4String(docID)) {
                using(var retVal = NativeRaw.c4view_fullTextMatched(view, docID_.AsC4Slice(), seq, fullTextID, outError)) {
                    return retVal.CreateString();
                }
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
        public static extern C4View* c4view_open(C4Database* database,
                                                 C4Slice path,
                                                 C4Slice viewName,
                                                 C4Slice version,
                                                 C4DatabaseConfig* config,
                                                 C4Error* outError);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4view_deleteAtPath(C4Slice path, C4DatabaseConfig* config, C4Error* outError);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4view_deleteByName(C4Database* database, C4Slice name, C4Error* outError);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void c4view_setMapVersion(C4View* view, C4Slice version);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void c4view_setDocumentType(C4View* view, C4Slice docType);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern C4Indexer* c4indexer_begin(C4Database* db, C4View** views, UIntPtr viewCount, C4Error* outError);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4indexer_emit(C4Indexer* indexer,
                                                 C4Document* document,
                                                 uint viewNumber,
                                                 uint emitCount,
                                                 C4Key** emittedKeys,
                                                 C4Slice[] emittedValues,
                                                 C4Error* outError);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern C4QueryEnumerator* c4view_fullTextQuery(C4View* view,
                                                                     C4Slice queryString,
                                                                     C4Slice queryStringLanguage,
                                                                     C4QueryOptions* c4Options,
                                                                     C4Error* outError);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern C4Slice c4queryenum_fullTextMatched(C4QueryEnumerator* e);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern C4Slice c4view_fullTextMatched(C4View* view,
                                                            C4Slice docID,
                                                            C4SequenceNumber seq,
                                                            uint fullTextID,
                                                            C4Error* outError);
    }
}
