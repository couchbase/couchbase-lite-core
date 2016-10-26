//
// Document.cs
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
using C4SequenceNumber = System.UInt64;

namespace LiteCore.Interop
{
    [Flags]
    public enum C4DocumentFlags : uint
    {
        Deleted = 0x01,
        Conflicted = 0x02,
        HasAttachments = 0x04,
        Exists = 0x1000
    }

    [Flags]
    public enum C4RevisionFlags : byte
    {
        Deleted = 0x01,
        Leaf = 0x02,
        New = 0x04,
        HasAttachments = 0x08
    }

    public struct C4Revision
    {
        public C4Slice revID;
        public C4RevisionFlags flags;
        public C4SequenceNumber sequence;
        public C4Slice body;
    }

    public struct C4Document
    {
        public C4DocumentFlags flags;
        public C4Slice docID;
        public C4Slice revID;
        public C4SequenceNumber sequence;
        public C4Revision selectedRev;
    }

    public unsafe struct C4DocPutRequest
    {
        public C4Slice body;
        public C4Slice docID;
        public C4Slice docType;
        private byte _deletion;
        private byte _hasAttachments;
        private byte _existingRevision;
        private byte _allowConflict;
        public C4Slice* history;
        private UIntPtr _historyCount;
        private byte _save;
        public uint maxRevTreeDepth;

        public bool deletion
        {
            get {
                return Convert.ToBoolean(_deletion);
            }
            set {
                _deletion = Convert.ToByte(value);
            }
        }

        public bool hasAttachments
        {
            get {
                return Convert.ToBoolean(_hasAttachments);
            }
            set {
                _hasAttachments = Convert.ToByte(value);
            }
        }

        public bool existingRevision
        {
            get {
                return Convert.ToBoolean(_existingRevision);
            }
            set {
                _existingRevision = Convert.ToByte(value);
            }
        }

        public bool allowConflict
        {
            get {
                return Convert.ToBoolean(_allowConflict);
            }
            set {
                _allowConflict = Convert.ToByte(value);
            }
        }

        public ulong historyCount
        {
            get {
                return _historyCount.ToUInt64();
            }
            set {
                _historyCount = (UIntPtr)value;
            }
        }

        public bool save
        {
            get {
                return Convert.ToBoolean(_save);
            }
            set {
                _save = Convert.ToByte(value);
            }
        }
    }

    public unsafe static partial class Native
    {
        public static C4Document* c4doc_get(C4Database* database, string docID, bool mustExist, C4Error* outError)
        {
            using(var docID_ = new C4String(docID)) {
                return NativeRaw.c4doc_get(database, docID_.AsC4Slice(), mustExist, outError);
            }
        }

        public static C4Document* c4doc_getForPut(C4Database *database,
                                                  string docID,
                                                  string parentRevID,
                                                  bool deleting,
                                                  bool allowConflict,
                                                  C4Error *outError)
        {
            using(var docID_ = new C4String(docID))
            using(var parentRevID_ = new C4String(parentRevID)) {
                return NativeRaw.c4doc_getForPut(database, docID_.AsC4Slice(), parentRevID_.AsC4Slice(),
                    deleting, allowConflict, outError);
            }
        }

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern C4Document* c4doc_getBySequence(C4Database* database, C4SequenceNumber sequence, C4Error* outError);

        public static string c4doc_getType(C4Document* doc)
        {
            using(var retVal = NativeRaw.c4doc_getType(doc)) {
                return ((C4Slice)retVal).CreateString();
            }
        }

        public static void c4doc_setType(C4Document* doc, string docType)
        {
            using(var docType_ = new C4String(docType)) {
                NativeRaw.c4doc_setType(doc, docType_.AsC4Slice());
            }
        }

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4doc_save(C4Document* doc, uint maxRevTreeDepth, C4Error* outError);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void c4doc_free(C4Document* doc);

        public static bool c4doc_selectRevision(C4Document* doc, string revID, bool withBody, C4Error* outError)
        {
            using(var revID_ = new C4String(revID)) {
                return NativeRaw.c4doc_selectRevision(doc, revID_.AsC4Slice(), withBody, outError);
            }
        }

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4doc_selectCurrentRevision(C4Document* doc);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4doc_loadRevisionBody(C4Document* doc, C4Error* outError);

        public static string c4doc_detachRevisionBody(C4Document* doc)
        {
            using(var retVal = NativeRaw.c4doc_detachRevisionBody(doc)) {
                return ((C4Slice)retVal).CreateString();
            }
        }

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4doc_hasRevisionBody(C4Document* doc);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4doc_selectParentRevision(C4Document* doc);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4doc_selectNextRevision(C4Document* doc);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4doc_selectNextLeafRevision(C4Document* doc,
                                                              [MarshalAs(UnmanagedType.U1)]bool includeDeleted,
                                                              [MarshalAs(UnmanagedType.U1)]bool withBody,
                                                               C4Error* outError);

        public static uint c4rev_getGeneration(string revID)
        {
            using(var revID_ = new C4String(revID)) {
                return NativeRaw.c4rev_getGeneration(revID_.AsC4Slice());
            }
        }

        public static int c4doc_purgeRevision(C4Document *doc, string revID, C4Error *outError)
        {
            using(var revID_ = new C4String(revID)) {
                return NativeRaw.c4doc_purgeRevision(doc, revID_.AsC4Slice(), outError);
            }
        }

        public static bool c4db_purgeDoc(C4Database *database, string docID, C4Error *outError)
        {
            using(var docID_ = new C4String(docID)) {
                return NativeRaw.c4db_purgeDoc(database, docID_.AsC4Slice(), outError);
            }
        }

        public static bool c4doc_setExpiration(C4Database *db, string docID, ulong expiration, C4Error *outError)
        {
            using(var docID_ = new C4String(docID)) {
                return NativeRaw.c4doc_setExpiration(db, docID_.AsC4Slice(), expiration, outError);
            }
        }

        public static ulong c4doc_getExpiration(C4Database* database, string docID)
        {
            using(var docID_ = new C4String(docID)) {
                return NativeRaw.c4doc_getExpiration(database, docID_.AsC4Slice());
            }
        }

        public static C4Document* c4doc_put(C4Database *database, 
                                            C4DocPutRequest *request, 
                                            ulong* outCommonAncestorIndex, 
                                            C4Error *outError)
        {
            var uintptr = new UIntPtr();
            var retVal = NativeRaw.c4doc_put(database, request, &uintptr, outError);
            if(outCommonAncestorIndex != null) {
                *outCommonAncestorIndex = uintptr.ToUInt64();
            }

            return retVal;
        }

        public static string c4doc_generateRevID(string body, string parentRevID, bool deletion)
        {
            using(var body_ = new C4String(body))
            using(var parentRevID_ = new C4String(parentRevID)) {
                using(var retVal = NativeRaw.c4doc_generateRevID(body_.AsC4Slice(), parentRevID_.AsC4Slice(), deletion)) {
                    return ((C4Slice)retVal).CreateString();
                }
            }
        }

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void c4doc_generateOldStyleRevID([MarshalAs(UnmanagedType.U1)]bool generateOldStyle);
    
        public static string c4doc_bodyAsJSON(C4Document *doc, C4Error *outError)
        {
            using(var retVal = NativeRaw.c4doc_bodyAsJSON(doc, outError)) {
                return ((C4Slice)retVal).CreateString();
            }
        }

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern FLEncoder* c4db_createFleeceEncoder(C4Database* db);

        public static string c4db_encodeJSON(C4Database* db, string jsonData, C4Error *outError)
        {
            using(var jsonData_ = new C4String(jsonData)) {
                using(var retVal = NativeRaw.c4db_encodeJSON(db, jsonData_.AsC4Slice(), outError)) {
                    return ((C4Slice)retVal).CreateString();
                }
            }
        }

        public static FLDictKey c4db_initFLDictKey(C4Database *db, string str)
        {
            using(var str_ = new C4String(str)) {
                return NativeRaw.c4db_initFLDictKey(db, str_.AsC4Slice());
            }
        }
    }


    public unsafe static partial class NativeRaw
    {
        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern C4Document* c4doc_get(C4Database* database,
                                                   C4Slice docID,
                                                   [MarshalAs(UnmanagedType.U1)]bool mustExist,
                                                   C4Error* outError);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern C4Document* c4doc_getForPut(C4Database *database,
                                                        C4Slice docID,
                                                        C4Slice parentRevID,
                                                        [MarshalAs(UnmanagedType.U1)]bool deleting,
                                                        [MarshalAs(UnmanagedType.U1)]bool allowConflict,
                                                        C4Error *outError);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern C4SliceResult c4doc_getType(C4Document* doc);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void c4doc_setType(C4Document* doc, C4Slice docType);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4doc_selectRevision(C4Document* doc,
                                                       C4Slice revID,
                                                      [MarshalAs(UnmanagedType.U1)]bool withBody,
                                                       C4Error* outError);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern C4SliceResult c4doc_detachRevisionBody(C4Document* doc);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern uint c4rev_getGeneration(C4Slice revID);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int c4doc_purgeRevision(C4Document* doc, C4Slice revID, C4Error* outError);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4db_purgeDoc(C4Database* database, C4Slice docID, C4Error* outError);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4doc_setExpiration(C4Database* database,
                                                     C4Slice docID,
                                                     ulong timestamp,
                                                      C4Error* outError);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern ulong c4doc_getExpiration(C4Database* database, C4Slice docID);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern C4Document* c4doc_put(C4Database* database,
                                                   C4DocPutRequest* request,
                                                   UIntPtr* outCommonAncestorIndex,
                                                   C4Error* outError);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern C4SliceResult c4doc_generateRevID(C4Slice body, C4Slice parentRevID, [MarshalAs(UnmanagedType.U1)]bool deletion);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern C4SliceResult c4doc_bodyAsJSON(C4Document *doc, C4Error *outError);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern C4SliceResult c4db_encodeJSON(C4Database* db, C4Slice jsonData, C4Error *outError);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern FLDictKey c4db_initFLDictKey(C4Database *db, C4Slice str);

    }
}
