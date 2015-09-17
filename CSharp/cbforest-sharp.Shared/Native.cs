//
//  Native.cs
//
//  Author:
//  	Jim Borden  <jim.borden@couchbase.com>
//
//  Copyright (c) 2015 Couchbase, Inc All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License");
//  you may not use this file except in compliance with the License.
//  You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.
//
using System;
using System.Linq;
using System.Runtime.InteropServices;

#if __IOS__
[assembly: ObjCRuntime.LinkWith("libCBForest-Interop.a", 
    ObjCRuntime.LinkTarget.Arm64 | ObjCRuntime.LinkTarget.ArmV7 | ObjCRuntime.LinkTarget.ArmV7s, ForceLoad=true,
    LinkerFlags="-lsqlite3 -lc++", Frameworks="", IsCxx=true)]
#endif

namespace CBForest
{
    public static unsafe class Native
    {
#if __IOS__
        private const string DLL_NAME = "__Internal";
#else
        private const string DLL_NAME = "CBForest.dll";
#endif
        
        [DllImport("msvcrt.dll", CallingConvention=CallingConvention.Cdecl)]
        public static extern int memcmp(void* b1, void* b2, UIntPtr count);
        
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        public static extern void c4slice_free(C4Slice slice);
        
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        private static extern C4Database* c4db_open(C4Slice path, byte readOnly, C4Error *outError);
        
        public static C4Database *c4db_open(string path, bool readOnly, C4Error *outError)
        {
            using(var path_ = new C4String(path)) {
                return c4db_open(path_.AsC4Slice(), Convert.ToByte(readOnly), outError);
            }
        }
        
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi, EntryPoint="c4db_close")]
        private static extern byte _c4db_close(C4Database *db, C4Error *outError);
        
        public static bool c4db_close(C4Database *db, C4Error *outError)
        {
            return Convert.ToBoolean(_c4db_close(db, outError));   
        }
        
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi, EntryPoint="c4db_delete")]
        private static extern byte _c4db_delete(C4Database *db, C4Error *outError);
        
        public static bool c4db_delete(C4Database *db, C4Error *outError)
        {
            return Convert.ToBoolean(_c4db_delete(db, outError));
        }
        
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        public static extern ulong c4db_getDocumentCount(C4Database *db);
        
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        public static extern ulong c4db_getLastSequence(C4Database *db);
     
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi, EntryPoint="c4db_beginTransaction")]
        private static extern byte _c4db_beginTransaction(C4Database *db, C4Error *outError);
        
        public static bool c4db_beginTransaction(C4Database *db, C4Error *outError)
        {
            return Convert.ToBoolean(_c4db_beginTransaction(db, outError));
        }
        
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        private static extern byte c4db_endTransaction(C4Database *db, byte commit, C4Error *outError);
        
        public static bool c4db_endTransaction(C4Database *db, bool commit, C4Error *outError)
        {
            return Convert.ToBoolean(c4db_endTransaction(db, Convert.ToByte(commit), outError));
        }
        
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi, EntryPoint="c4db_isInTransaction")]
        private static extern byte _c4db_isInTransaction(C4Database *db);
        
        public static bool c4db_isInTransaction(C4Database *db)
        {
            return Convert.ToBoolean(_c4db_isInTransaction(db));   
        }
        
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        public static extern void c4raw_free(C4RawDocument *rawDoc);
        
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        private static extern C4RawDocument* c4raw_get(C4Database *db, C4Slice storeName, C4Slice docID, C4Error *outError);
        
        public static C4RawDocument* c4raw_get(C4Database *db, string storeName, string docID, C4Error *outError)
        {
            using(var storeName_ = new C4String(storeName))
            using(var docID_ = new C4String(docID)) {
                return c4raw_get(db, storeName_.AsC4Slice(), docID_.AsC4Slice(), outError);
            }
        }
        
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        private static extern byte c4raw_put(C4Database *db, C4Slice storeName, C4Slice key, C4Slice meta,
            C4Slice body, C4Error *outError);
        
        public static bool c4raw_put(C4Database *db, string storeName, string key, string meta, string body, C4Error *outError)
        {
            using(var storeName_ = new C4String(storeName))
            using(var key_ = new C4String(key))
            using(var meta_ = new C4String(meta))
            using(var body_ = new C4String(body)) {
                return Convert.ToBoolean(c4raw_put(db, storeName_.AsC4Slice(), key_.AsC4Slice(), meta_.AsC4Slice(), 
                                body_.AsC4Slice(), outError));  
            }
        }
        
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        public static extern void c4doc_free(C4Document *doc);
        
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        private static extern C4Document* c4doc_get(C4Database *db, C4Slice docID, byte mustExist, C4Error *outError);
        
        public static C4Document* c4doc_get(C4Database *db, string docID, bool mustExist, C4Error *outError)
        {
            using(var docID_ = new C4String(docID)) {
                return c4doc_get(db, docID_.AsC4Slice(), Convert.ToByte(mustExist), outError);   
            }
        }
        
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi, EntryPoint="c4doc_getType")]
        private static extern C4Slice _c4doc_getType(C4Document *doc);
        
        public static string c4doc_getType(C4Document *doc)
        {
            var rawRetVal = _c4doc_getType(doc);
            var retVal = (string)rawRetVal;
            c4slice_free(rawRetVal);
            return retVal;
        }
        
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        private static extern byte c4doc_selectRevision(C4Document *doc, C4Slice revID, byte withBody, C4Error *outError);
        
        public static bool c4doc_selectRevision(C4Document *doc, string revID, bool withBody, C4Error *outError)
        {
            using(var revID_ = new C4String(revID)) {
                return Convert.ToBoolean(c4doc_selectRevision(doc, revID_.AsC4Slice(), Convert.ToByte(withBody), outError));
            }
        }
        
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi, EntryPoint="c4doc_selectCurrentRevision")]
        private static extern byte _c4doc_selectCurrentRevision(C4Document *doc);
        
        public static bool c4doc_selectCurrentRevision(C4Document *doc)
        {
            return Convert.ToBoolean(_c4doc_selectCurrentRevision(doc)); 
        }
        
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi, EntryPoint="c4doc_loadRevisionBody")]
        private static extern byte _c4doc_loadRevisionBody(C4Document *doc, C4Error *outError);
        
        public static bool c4doc_loadRevisionBody(C4Document *doc, C4Error *outError)
        {
            return Convert.ToBoolean(_c4doc_loadRevisionBody(doc, outError));
        }
        
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi, EntryPoint="c4doc_selectParentRevision")]
        private static extern byte _c4doc_selectParentRevision(C4Document *doc);
        
        public static bool c4doc_selectParentRevision(C4Document *doc)
        {
            return Convert.ToBoolean(_c4doc_selectParentRevision(doc));
        }
        
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi, EntryPoint="c4doc_selectNextRevision")]
        private static extern byte _c4doc_selectNextRevision(C4Document *doc);
        
        public static bool c4doc_selectNextRevision(C4Document *doc)
        {
            return Convert.ToBoolean(_c4doc_selectNextRevision(doc));
        }
        
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        private static extern byte c4doc_selectNextLeafRevision(C4Document *doc, byte includeDeleted, byte withBody, C4Error *outError);
        
        public static bool c4doc_selectNextLeafRevision(C4Document *doc, bool includeDeleted, bool withBody, C4Error *outError)
        {
            return Convert.ToBoolean(c4doc_selectNextLeafRevision(doc, Convert.ToByte(includeDeleted), Convert.ToByte(withBody),
                outError));
        }
        
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        public static extern void c4enum_free(C4DocEnumerator *e);
        
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        private static extern C4DocEnumerator* c4db_enumerateChanges(C4Database *db, ulong since, byte withBodies, C4Error *outError);
        
        public static C4DocEnumerator* c4db_enumerateChanges(C4Database *db, ulong since, bool withBodies, C4Error *outError)
        {
            return c4db_enumerateChanges(db, since, Convert.ToByte(withBodies), outError);
        }
        
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        private static extern C4DocEnumerator* c4db_enumerateAllDocs(C4Database *db, C4Slice startDocID, C4Slice endDocID, byte descending,
            byte inclusiveEnd, uint skip, byte withBodies, C4Error *outError);
        
        public static C4DocEnumerator* c4db_enumerateAllDocs(C4Database *db, string startDocID, string endDocID, bool descending,
            bool inclusiveEnd, uint skip, bool withBodies, C4Error *outError)
        {
            using(var startDocID_ = new C4String(startDocID))
            using(var endDocID_ = new C4String(endDocID)) {
                return c4db_enumerateAllDocs(db, startDocID_.AsC4Slice(), endDocID_.AsC4Slice(), Convert.ToByte(descending), 
                    Convert.ToByte(inclusiveEnd), skip, Convert.ToByte(withBodies), outError);
            }
        }
        
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        public static extern C4Document* c4enum_nextDocument(C4DocEnumerator *e, C4Error *outError);
        
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        private static extern byte c4doc_insertRevision(C4Document *doc, C4Slice revID, C4Slice body, byte deleted, byte hasAttachments,
            byte allowConflict, C4Error *outError);
        
        public static bool c4doc_insertRevision(C4Document *doc, string revID, string body, bool deleted, bool hasAttachments,
            bool allowConflict, C4Error *outError)
        {
            using(var revID_ = new C4String(revID))
            using(var body_ = new C4String(body)) {
                return Convert.ToBoolean(c4doc_insertRevision(doc, revID_.AsC4Slice(), body_.AsC4Slice(), 
                    Convert.ToByte(deleted), Convert.ToByte(hasAttachments), Convert.ToByte(allowConflict), outError));
            }
        }
        
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        private static extern int c4doc_insertRevisionWithHistory(C4Document *doc, C4Slice revID, C4Slice body, byte deleted, 
            byte hasAttachments, C4Slice[] history, uint historyCount, C4Error *outError);
        
        public static int c4doc_insertRevisionWithHistory(C4Document *doc, string revID, string body, bool deleted, 
            bool hasAttachments, string[] history, uint historyCount, C4Error *outError)
        {
            var flattenedStringArray = new C4String[history.Length + 2];
            flattenedStringArray[0] = new C4String(revID);
            flattenedStringArray[1] = new C4String(body);
            for(int i = 0; i < historyCount; i++) {
                flattenedStringArray[i + 2] = new C4String(history[i]);
            }
            
            var retVal = c4doc_insertRevisionWithHistory(doc, flattenedStringArray[0].AsC4Slice(), flattenedStringArray[1].AsC4Slice(), 
                Convert.ToByte(deleted), Convert.ToByte(hasAttachments), 
                flattenedStringArray.Skip(2).Select<C4String, C4Slice>(x => x.AsC4Slice()).ToArray(), historyCount, outError);
            
            foreach(var s in flattenedStringArray) {
                s.Dispose();   
            }
            
            return retVal;
        }
        
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        private static extern byte c4doc_setType(C4Document *doc, C4Slice docType, C4Error *outError);
        
        public static bool c4doc_setType(C4Document *doc, string docType, C4Error *outError)
        {
            using(var docType_ = new C4String(docType)) {
                return Convert.ToBoolean(c4doc_setType(doc, docType_.AsC4Slice(), outError));   
            }
        }
        
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi, EntryPoint="c4doc_save")]
        private static extern byte _c4doc_save(C4Document *doc, uint maxRevTreeDepth, C4Error *outError);
        
        public static bool c4doc_save(C4Document *doc, uint maxRevTreeDepth, C4Error *outError)
        {
            return Convert.ToBoolean(_c4doc_save(doc, maxRevTreeDepth, outError));
        }
    }
}

