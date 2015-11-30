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
//#define ENABLE_LOGGING
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Linq;
using System.Runtime.InteropServices;

#if __IOS__
[assembly: ObjCRuntime.LinkWith("libCBForest-Interop.a", 
    ObjCRuntime.LinkTarget.Arm64 | ObjCRuntime.LinkTarget.ArmV7 | ObjCRuntime.LinkTarget.ArmV7s, ForceLoad=true,
    LinkerFlags="-lsqlite3 -lc++", Frameworks="Security", IsCxx=true)]
#endif

namespace CBForest
{
    /// <summary>
    /// Bridge into CBForest-Interop.dll
    /// </summary>
    public static unsafe class Native
    {
#if __IOS__
        private const string DLL_NAME = "__Internal";
#else
        private const string DLL_NAME = "CBForest-Interop";
#endif

        #if DEBUG && !NET_3_5
        private static string _Dummy;
        private static readonly System.Collections.Concurrent.ConcurrentDictionary<IntPtr, string> _AllocatedObjects = new 
            System.Collections.Concurrent.ConcurrentDictionary<IntPtr, string>();
        private static readonly Dictionary<string, Action<IntPtr>> _GcAction = new Dictionary<string, Action<IntPtr>>
        {
            { "C4Database", p => _c4db_close((C4Database*)p.ToPointer(), null) },
            { "C4RawDocument", p => _c4raw_free((C4RawDocument*)p.ToPointer()) },
            { "C4Document", p => _c4doc_free((C4Document*)p.ToPointer()) },
            { "C4DocEnumerator", p => _c4enum_free((C4DocEnumerator*)p.ToPointer()) },
            { "C4Key", p => _c4key_free((C4Key*)p.ToPointer()) },
            { "C4View", p => _c4view_close((C4View*)p.ToPointer(), null) },
            { "C4Indexer", p => _c4indexer_end((C4Indexer*)p.ToPointer(), false, null) },
            { "C4QueryEnumerator", p => _c4queryenum_free((C4QueryEnumerator*)p.ToPointer()) }
        };
        #endif

        private static Action<C4LogLevel, string> _LogCallback;
        private static C4LogCallback _NativeLogCallback;

        [Conditional("DEBUG")]
        public static void CheckMemoryLeaks()
        {
            #if DEBUG && !NET_3_5
            foreach (var pair in _AllocatedObjects) {
                Console.WriteLine("ERROR: {0}* at 0x{1} leaked", pair.Value, pair.Key.ToString("X"));
                _GcAction[pair.Value](pair.Key); // To make sure the next test doesn't fail because of this one's mistakes
            }

            if (_AllocatedObjects.Any()) {
                _AllocatedObjects.Clear();
                throw new ApplicationException("Memory leaks detected");
            }
            #endif
        }

        /// <summary>
        /// Compares the first count bytes of the block of memory pointed by b1 to the
        /// first count bytes pointed by b2. returning zero if they all match or a value 
        /// different from zero representing which is greater if they do not.
        /// </summary>
        /// <param name="b1">Pointer to block of memory.</param>
        /// <param name="b2">Pointer to block of memory.</param>
        /// <param name="count">Number of bytes to compare.</param>
        /// <returns>Returns an integral value indicating the relationship between the content of the memory blocks:
        /// return value    |   indicates
        /// %lt;0	        |   the first byte that does not match in both memory blocks has a lower value in b1 than in v2
        ///                     (if evaluated as unsigned char values)
        /// 0	            |   the contents of both memory blocks are equal
        /// &gt;0	        |   the first byte that does not match in both memory blocks has a greater value in b1 than in b2
        ///                     (if evaluated as unsigned char values)</returns>
        [DllImport("msvcrt.dll", CallingConvention=CallingConvention.Cdecl)]
        public static extern int memcmp(void* b1, void* b2, UIntPtr count);

        [DllImport("msvcrt.dll", CallingConvention=CallingConvention.Cdecl)]
        public static extern int memcpy(void* dest, void* src, UIntPtr count);

        /// <summary>
        /// Frees the memory of a C4Slice.
        /// </summary>
        /// <param name="slice">The slice that will have its memory freed</param>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        public static extern void c4slice_free(C4Slice slice);
        
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi, EntryPoint="c4db_open")]
        private static extern C4Database* _c4db_open(C4Slice path, C4DatabaseFlags flags, 
            C4EncryptionKey *encryptionKey, C4Error *outError);

        /// <summary>
        /// Opens a database.
        /// </summary>
        /// <param name="path">The path to the DB file</param>
        /// <param name="readOnly">Whether or not the DB should be opened in read-only mode</param>
        /// <param name="encryptionKey">The option encryption key used to encrypt the database</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>A database instance for use in the C4 API</returns>
        public static C4Database* c4db_open(C4Slice path, C4DatabaseFlags flags, 
            C4EncryptionKey *encryptionKey, C4Error *outError)
        {
            #if DEBUG && !NET_3_5
            var retVal = _c4db_open(path, flags, encryptionKey, outError);
            if(retVal != null) {
                _AllocatedObjects.TryAdd((IntPtr)retVal, "C4Database");
                #if ENABLE_LOGGING
                Console.WriteLine("[c4db_open] Allocated 0x{0}", ((IntPtr)retVal).ToString("X"));
                #endif
            }

            return retVal;
            #else
            return _c4db_open(path, flags, encryptionKey, outError);
            #endif
        }

        /// <summary>
        /// Opens a database.
        /// </summary>
        /// <param name="path">The path to the DB file</param>
        /// <param name="readOnly">Whether or not the DB should be opened in read-only mode</param>
        /// <param name="encryptionKey">The option encryption key used to encrypt the database</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>A database instance for use in the C4 API</returns>
        public static C4Database *c4db_open(string path, C4DatabaseFlags flags,
            C4EncryptionKey *encryptionKey, C4Error *outError)
        {
            using(var path_ = new C4String(path)) {
                return c4db_open(path_.AsC4Slice(), flags, encryptionKey, outError);
            }
        }

        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi, EntryPoint="c4db_close")]
        [return: MarshalAs(UnmanagedType.U1)]
        private static extern bool _c4db_close(C4Database *db, C4Error *outError);

        /// <summary>
        /// Closes the database and frees the object.
        /// </summary>
        /// <param name="db">The DB object to close</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>true on success, false otherwise</returns>
        public static bool c4db_close(C4Database *db, C4Error *outError)
        {
            #if DEBUG && !NET_3_5
            var ptr = (IntPtr)db;
            #if ENABLE_LOGGING
            if(ptr != IntPtr.Zero && !_AllocatedObjects.ContainsKey(ptr)) {
                Console.WriteLine("WARNING: [c4db_close] freeing object 0x{0} that was not found in allocated list", ptr.ToString("X"));
            } else {
            #endif
                _AllocatedObjects.TryRemove(ptr, out _Dummy);
            #if ENABLE_LOGGING
            }
            #endif
            #endif
            return _c4db_close(db, outError);
        }
            
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi, EntryPoint="c4db_delete")]
        [return: MarshalAs(UnmanagedType.U1)]
        private static extern bool _c4db_delete(C4Database *db, C4Error *outError);

        /// <summary>
        /// Closes the database, deletes the file, and frees the object
        /// </summary>
        /// <param name="db">The DB object to delete</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>true on success, false otherwise</returns>
        public static bool c4db_delete(C4Database *db, C4Error *outError)
        {
            #if DEBUG && !NET_3_5
            var ptr = (IntPtr)db;
            #if ENABLE_LOGGING
            if(ptr != IntPtr.Zero && !_AllocatedObjects.ContainsKey(ptr)) {
                Console.WriteLine("WARNING: [c4db_delete] freeing object 0x{0} that was not found in allocated list", ptr.ToString("X"));
            } else {
            #endif
                _AllocatedObjects.TryRemove(ptr, out _Dummy);
            #if ENABLE_LOGGING
            }
            #endif
            #endif
            return _c4db_delete(db, outError);
        }


        /// <summary>
        /// Triggers a manual compaction of the database
        /// </summary>
        /// <returns>true on success, false otherwise</returns>
        /// <param name="db">The database to compact.</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4db_compact(C4Database *db, C4Error *outError);

        /// <summary>
        /// Changes the encryption key of a given database
        /// </summary>
        /// <returns>true on success, false otherwise</returns>
        /// <param name="db">The database to change the encryption key of</param>
        /// <param name="newKey">The new encryption key to use</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4db_rekey(C4Database *db, C4EncryptionKey *newKey, C4Error *outError);

        /// <summary>
        /// Returns the number of (undeleted) documents in the database.
        /// </summary>
        /// <param name="db">The database object to examine</param>
        /// <returns>The number of (undeleted) documents in the database.</returns>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        public static extern ulong c4db_getDocumentCount(C4Database *db);

        /// <summary>
        /// Returns the latest sequence number allocated to a revision.
        /// </summary>
        /// <param name="db">The database object to examine</param>
        /// <returns>The latest sequence number allocated to a revision.</returns>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        public static extern ulong c4db_getLastSequence(C4Database *db);

        /// <summary>
        /// Begins a transaction.
        /// Transactions can nest; only the first call actually creates a ForestDB transaction.
        /// </summary>
        /// <param name="db">The database object to start a transaction on</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>true on success, false otherwise</returns>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4db_beginTransaction(C4Database *db, C4Error *outError);

        /// <summary>
        /// Commits or aborts a transaction. If there have been multiple calls to beginTransaction, 
        /// it takes the same number of calls to endTransaction to actually end the transaction; only 
        /// the last one commits or aborts the ForestDB transaction.
        /// </summary>
        /// <param name="db">The database to end a transaction on</param>
        /// <param name="commit">If true, commit the results, otherwise abort</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>true on success, false otherwise</returns>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4db_endTransaction(C4Database *db, [MarshalAs(UnmanagedType.U1)]bool commit, C4Error *outError);

        /// <summary>
        /// Is a transaction active?
        /// </summary>
        /// <param name="db">The database to check</param>
        /// <returns>Whether or not a transaction is active</returns>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4db_isInTransaction(C4Database *db);

        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi, EntryPoint="c4raw_free")]
        private static extern void _c4raw_free(C4RawDocument *rawDoc);

        /// <summary>
        /// Frees the storage occupied by a raw document.
        /// </summary>
        /// <param name="rawDoc"></param>
        public static void c4raw_free(C4RawDocument *rawDoc)
        {
            #if DEBUG && !NET_3_5
            var ptr = (IntPtr)rawDoc;
            #if ENABLE_LOGGING
            if(ptr != IntPtr.Zero && !_AllocatedObjects.ContainsKey(ptr)) {
                Console.WriteLine("WARNING: [c4db_delete] freeing object 0x{0} that was not found in allocated list", ptr.ToString("X"));
            } else {
            #endif
                _AllocatedObjects.TryRemove(ptr, out _Dummy);
            #if ENABLE_LOGGING
            }
            #endif
            #endif
            _c4raw_free(rawDoc);
        }
        
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi, EntryPoint="c4raw_get")]
        private static extern C4RawDocument* _c4raw_get(C4Database *db, C4Slice storeName, C4Slice docID, C4Error *outError);

        /// <summary>
        /// Reads a raw document from the database. In Couchbase Lite the store named "info" is used for 
        /// per-database key/value pairs, and the store "_local" is used for local documents.
        /// </summary>
        /// <param name="db">The database to operate on</param>
        /// <param name="storeName">The name of the store to read from</param>
        /// <param name="docID">The ID of the document to retrieve</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>A pointer to the retrieved document on success, or null on failure</returns>
        public static C4RawDocument* c4raw_get(C4Database *db, C4Slice storeName, C4Slice docID, C4Error *outError)
        {
            #if DEBUG && !NET_3_5
            var retVal = _c4raw_get(db, storeName, docID, outError);
            if(retVal != null) {
                _AllocatedObjects.TryAdd((IntPtr)retVal, "C4RawDocument");
                #if ENABLE_LOGGING
                Console.WriteLine("[c4raw_get] Allocated 0x{0}", ((IntPtr)retVal).ToString("X"));
                #endif
            }

            return retVal;
            #else
            return _c4raw_get(db, storeName, docID, outError);
            #endif
        }

        /// <summary>
        /// Reads a raw document from the database. In Couchbase Lite the store named "info" is used for 
        /// per-database key/value pairs, and the store "_local" is used for local documents.
        /// </summary>
        /// <param name="db">The database to operate on</param>
        /// <param name="storeName">The name of the store to read from</param>
        /// <param name="docID">The ID of the document to retrieve</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>A pointer to the retrieved document on success, or null on failure</returns>
        public static C4RawDocument* c4raw_get(C4Database *db, string storeName, string docID, C4Error *outError)
        {
            using(var storeName_ = new C4String(storeName))
            using(var docID_ = new C4String(docID)) {
                return c4raw_get(db, storeName_.AsC4Slice(), docID_.AsC4Slice(), outError);
            }
        }

        /// <summary>
        /// Writes a raw document to the database, or deletes it if both meta and body are NULL.
        /// </summary>
        /// <param name="db">The database to operate on</param>
        /// <param name="storeName">The store to write to</param>
        /// <param name="key">The key to store</param>
        /// <param name="meta">The metadata to store</param>
        /// <param name="body">The body to store</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>true on success, false otherwise</returns>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4raw_put(C4Database *db, C4Slice storeName, C4Slice key, C4Slice meta,
            C4Slice body, C4Error *outError);

        /// <summary>
        /// Writes a raw document to the database, or deletes it if both meta and body are NULL.
        /// </summary>
        /// <param name="db">The database to operate on</param>
        /// <param name="storeName">The store to write to</param>
        /// <param name="key">The key to store</param>
        /// <param name="meta">The metadata to store</param>
        /// <param name="body">The body to store</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>true on success, false otherwise</returns>
        public static bool c4raw_put(C4Database *db, string storeName, string key, string meta, string body, C4Error *outError)
        {
            using(var storeName_ = new C4String(storeName))
            using(var key_ = new C4String(key))
            using(var meta_ = new C4String(meta))
            using(var body_ = new C4String(body)) {
                return c4raw_put(db, storeName_.AsC4Slice(), key_.AsC4Slice(), meta_.AsC4Slice(), 
                                body_.AsC4Slice(), outError);  
            }
        }

        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi, EntryPoint="c4doc_free")]
        private static extern void _c4doc_free(C4Document *doc);

        /// <summary>
        /// Frees a C4Document.
        /// </summary>
        /// <param name="doc">The document to free</param>
        public static void c4doc_free(C4Document *doc)
        {
            #if DEBUG && !NET_3_5
            var ptr = (IntPtr)doc;
            #if ENABLE_LOGGING
            if(ptr != IntPtr.Zero && !_AllocatedObjects.ContainsKey(ptr)) {
                Console.WriteLine("WARNING: [c4doc_free] freeing object 0x{0} that was not found in allocated list", ptr.ToString("X"));
            } else {
            #endif
                _AllocatedObjects.TryRemove(ptr, out _Dummy);
            #if ENABLE_LOGGING
            }
            #endif
            #endif
            _c4doc_free(doc);
        }
        
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi, EntryPoint="c4doc_get")]
        private static extern C4Document* _c4doc_get(C4Database *db, C4Slice docID, [MarshalAs(UnmanagedType.U1)]bool mustExist, 
            C4Error *outError);

        /// <summary>
        /// Gets a document from the database. If there's no such document, the behavior depends on
        /// the mustExist flag.If it's true, NULL is returned. If it's false, a valid C4Document
        /// The current revision is selected(if the document exists.)
        /// </summary>
        /// <param name="db">The database to retrieve from</param>
        /// <param name="docID">The ID of the document to retrieve</param>
        /// <param name="mustExist">Whether or not to create the document on demand</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>A pointer to the retrieved document on success, or null on failure</returns>
        public static C4Document* c4doc_get(C4Database *db, C4Slice docID, bool mustExist, 
            C4Error *outError)
        {
            #if DEBUG && !NET_3_5
            var retVal = _c4doc_get(db, docID, mustExist, outError);
            if(retVal != null) {
                _AllocatedObjects.TryAdd((IntPtr)retVal, "C4Document");
                #if ENABLE_LOGGING
                Console.WriteLine("[c4doc_get] Allocated 0x{0}", ((IntPtr)retVal).ToString("X"));
                #endif
            }

            return retVal;
            #else
            return _c4doc_get(db, docID, mustExist, outError);
            #endif
        }

        /// <summary>
        /// Gets a document from the database. If there's no such document, the behavior depends on
        /// the mustExist flag.If it's true, NULL is returned. If it's false, a valid C4Document
        /// The current revision is selected(if the document exists.)
        /// </summary>
        /// <param name="db">The database to retrieve from</param>
        /// <param name="docID">The ID of the document to retrieve</param>
        /// <param name="mustExist">Whether or not to create the document on demand</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>A pointer to the retrieved document on success, or null on failure</returns>
        public static C4Document* c4doc_get(C4Database *db, string docID, bool mustExist, C4Error *outError)
        {
            using(var docID_ = new C4String(docID)) {
                return c4doc_get(db, docID_.AsC4Slice(), mustExist, outError);   
            }
        }

        /// <summary>
        /// Retrieves a document by its sequence number
        /// </summary>
        /// <returns>The document identified by the sequence number given, or null</returns>
        /// <param name="db">The database to search in</param>
        /// <param name="sequence">The sequence number to search for</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        public static extern C4Document* c4doc_getBySequence(C4Database *db, ulong sequence, C4Error *outError);

        /// <summary>
        /// Returns the document type (as set by setDocType.) This value is ignored by CBForest itself; by convention 
        /// Couchbase Lite sets it to the value of the current revision's "type" property, and uses it as an optimization 
        /// when indexing a view.
        /// </summary>
        /// <param name="doc">The document to operate on</param>
        /// <returns>The document type of the document in question</returns>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi, EntryPoint="c4doc_getType")]
        public static extern C4Slice _c4doc_getType(C4Document *doc);

        /// <summary>
        /// Returns the document type (as set by setDocType.) This value is ignored by CBForest itself; by convention 
        /// Couchbase Lite sets it to the value of the current revision's "type" property, and uses it as an optimization 
        /// when indexing a view.
        /// </summary>
        /// <param name="doc">The document to operate on</param>
        /// <returns>The document type of the document in question</returns>
        public static string c4doc_getType(C4Document *doc)
        {
            return BridgeSlice(() => _c4doc_getType(doc));
        }

        /// <summary>
        /// Purges a document from the database.  When a purge occurs a document is immediately
        /// removed with no trace.  This action is *not* replicated.
        /// </summary>
        /// <returns>true on success, false otherwise</returns>
        /// <param name="db">The database to purge the document from</param>
        /// <param name="docId">The ID of the document to purge</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4db_purgeDoc(C4Database *db, C4Slice docId, C4Error *outError);

        /// <summary>
        /// Purges a document from the database.  When a purge occurs a document is immediately
        /// removed with no trace.  This action is *not* replicated.
        /// </summary>
        /// <returns>true on success, false otherwise</returns>
        /// <param name="db">The database to purge the document from</param>
        /// <param name="docId">The ID of the document to purge</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        public static bool c4db_purgeDoc(C4Database *db, string docId, C4Error *outError)
        {
            using (var docId_ = new C4String(docId)) {
                return c4db_purgeDoc(db, docId_.AsC4Slice(), outError);
            }
        }

        /// <summary>
        /// Selects a specific revision of a document (or no revision, if revID is NULL.)
        /// </summary>
        /// <param name="doc">The document to operate on</param>
        /// <param name="revID">The revID of the revision to select</param>
        /// <param name="withBody">Whether or not to load the body of the revision</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>true on success, false otherwise</returns>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4doc_selectRevision(C4Document *doc, C4Slice revID, [MarshalAs(UnmanagedType.U1)]bool withBody, 
            C4Error *outError);

        /// <summary>
        /// Selects a specific revision of a document (or no revision, if revID is NULL.)
        /// </summary>
        /// <param name="doc">The document to operate on</param>
        /// <param name="revID">The revID of the revision to select</param>
        /// <param name="withBody">Whether or not to load the body of the revision</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>true on success, false otherwise</returns>
        public static bool c4doc_selectRevision(C4Document *doc, string revID, bool withBody, C4Error *outError)
        {
            using(var revID_ = new C4String(revID)) {
                return c4doc_selectRevision(doc, revID_.AsC4Slice(), withBody, outError);
            }
        }

        /// <summary>
        /// Selects the current revision of a document. 
        /// </summary>
        /// <param name="doc">The document to operate on</param>
        /// <returns>true on success, false otherwise</returns>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4doc_selectCurrentRevision(C4Document *doc);

        /// <summary>
        /// Populates the body field of a doc's selected revision,
        /// if it was initially loaded without its body.
        /// </summary>
        /// <param name="doc">The document to operate on</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>true on success, false otherwise</returns>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4doc_loadRevisionBody(C4Document *doc, C4Error *outError);

        /// <summary>
        /// Checks to see if a given document has a revision body
        /// </summary>
        /// <returns><c>true</c>, if a revision body is available, <c>false</c> otherwise.</returns>
        /// <param name="doc">The document to check.</param>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4doc_hasRevisionBody(C4Document *doc);

        /// <summary>
        /// Selects the parent of the selected revision, if it's known, else inserts null.
        /// </summary>
        /// <param name="doc">The document to operate on</param>
        /// <returns>true on success, false otherwise</returns>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4doc_selectParentRevision(C4Document *doc);

        /// <summary>
        /// Selects the next revision in priority order.
        /// This can be used to iterate over all revisions, starting from the current revision.
        /// </summary>
        /// <param name="doc">The document to operate on</param>
        /// <returns>true on success, false otherwise</returns>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4doc_selectNextRevision(C4Document *doc);

        /// <summary>
        /// Selects the next leaf revision; like selectNextRevision but skips over non-leaves.
        /// </summary>
        /// <param name="doc">The document to operate on</param>
        /// <param name="includeDeleted">Whether or not to include deleted leafs</param>
        /// <param name="withBody">Whether or not to automatically load the body of the revision</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>true on success, false otherwise</returns>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4doc_selectNextLeafRevision(C4Document *doc, [MarshalAs(UnmanagedType.U1)]bool includeDeleted, 
            [MarshalAs(UnmanagedType.U1)]bool withBody, C4Error *outError);
        

        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi, EntryPoint="c4enum_free")]
        private static extern void _c4enum_free(C4DocEnumerator *e);

        /// <summary>
        /// Frees the resources used by a C4DocEnumerator
        /// </summary>
        /// <param name="e">The doc enumerator to free</param>
        public static void c4enum_free(C4DocEnumerator *e)
        {
            #if DEBUG && !NET_3_5
            var ptr = (IntPtr)e;
            #if ENABLE_LOGGING
            if(ptr != IntPtr.Zero && !_AllocatedObjects.ContainsKey(ptr)) {
                Console.WriteLine("WARNING: [c4enum_free] freeing object 0x{0} that was not found in allocated list", ptr.ToString("X"));
            } else {
            #endif
                _AllocatedObjects.TryRemove(ptr, out _Dummy);
            #if ENABLE_LOGGING
            }
            #endif
            #endif
            _c4enum_free(e);
        }

        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi, EntryPoint="c4db_enumerateChanges")]
        private static extern C4DocEnumerator* _c4db_enumerateChanges(C4Database* db, ulong since, C4EnumeratorOptions* options, 
            C4Error* outError);

        /// <summary>
        /// Creates an enumerator ordered by sequence.
        /// Caller is responsible for freeing the enumerator when finished with it.
        /// </summary>
        /// <param name="db">The database to operate on</param>
        /// <param name="since">The sequence number to start _after_.Pass 0 to start from the beginning.</param>
        /// <param name="options">Enumeration options (NULL for defaults).</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>A pointer to the enumeator on success, otherwise null</returns>
        public static C4DocEnumerator* c4db_enumerateChanges(C4Database* db, ulong since, C4EnumeratorOptions* options, 
            C4Error* outError)
        {
            #if DEBUG && !NET_3_5
            var retVal = _c4db_enumerateChanges(db, since, options, outError);
            if(retVal != null) {
                _AllocatedObjects.TryAdd((IntPtr)retVal, "C4DocEnumerator");
                #if ENABLE_LOGGING
                Console.WriteLine("[c4db_enumerateChanges] Allocated 0x{0}", ((IntPtr)retVal).ToString("X"));
                #endif
            }

            return retVal;
            #else
            return _c4db_enumerateChanges(db, since, options, outError);
            #endif
        }

        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi, EntryPoint="c4db_enumerateAllDocs")]
        private static extern C4DocEnumerator* _c4db_enumerateAllDocs(C4Database *db, C4Slice startDocID, C4Slice endDocID, 
            C4EnumeratorOptions *options, C4Error *outError);

        /// <summary>
        /// Creates an enumerator which will iterate over all documents in the database
        /// </summary>
        /// <returns>The enumerator object to use, or null on error</returns>
        /// <param name="db">The database to enumerate</param>
        /// <param name="startDocID">The document ID to start enumerating from</param>
        /// <param name="endDocID">The document ID to finish enumerating at</param>
        /// <param name="options">The enumeration options (null for defaults).</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        public static C4DocEnumerator* c4db_enumerateAllDocs(C4Database *db, C4Slice startDocID, C4Slice endDocID, 
            C4EnumeratorOptions *options, C4Error *outError)
        {
            #if DEBUG && !NET_3_5
            var retVal = _c4db_enumerateAllDocs(db, startDocID, endDocID, options, outError);
            if(retVal != null) {
                _AllocatedObjects.TryAdd((IntPtr)retVal, "C4DocEnumerator");
                #if ENABLE_LOGGING
                Console.WriteLine("[c4db_enumerateAllDocs] Allocated 0x{0}", ((IntPtr)retVal).ToString("X"));
                #endif
            }

            return retVal;
            #else
            return _c4db_enumerateAllDocs(db, startDocID, endDocID, options, outError);
            #endif
        }

        /// <summary>
        /// Creates an enumerator ordered by docID.
        /// Options have the same meanings as in Couchbase Lite.
        /// There's no 'limit' option; just stop enumerating when you're done.
        /// Caller is responsible for freeing the enumerator when finished with it.
        /// </summary>
        /// <param name="db">The database to operate on</param>
        /// <param name="startDocID">The document ID to begin at</param>
        /// <param name="endDocID">The document ID to end at</param>
        /// <param name="options">Enumeration options (NULL for defaults)</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>A pointer to the enumerator on success, otherwise null</returns>
        public static C4DocEnumerator* c4db_enumerateAllDocs(C4Database *db, string startDocID, string endDocID, C4EnumeratorOptions *options,
            C4Error *outError)
        {
            using(var startDocID_ = new C4String(startDocID))
            using(var endDocID_ = new C4String(endDocID)) {
                return c4db_enumerateAllDocs(db, startDocID_.AsC4Slice(), endDocID_.AsC4Slice(), options, outError);
            }
        }

        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi, EntryPoint="c4db_enumerateSomeDocs")]
        private static extern C4DocEnumerator* _c4db_enumerateSomeDocs(C4Database *db, C4Slice* docIDs, UIntPtr docIDsCount, 
            C4EnumeratorOptions *options, C4Error *outError);

        /// <summary>
        /// Enumerate a set of document IDs.  For each ID in the list, the enumerator
        /// will attempt to retrieve the corresponding document.
        /// </summary>
        /// <returns>The enumerator, or null on failure.</returns>
        /// <param name="db">The database to enumerate from</param>
        /// <param name="docIDs">The document IDs to enumerate</param>
        /// <param name="options">The enumeration options (null for defaults).</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        public static C4DocEnumerator* c4db_enumerateSomeDocs(C4Database *db, string[] docIDs, C4EnumeratorOptions *options,
            C4Error *outError)
        {
            var c4StringArr = docIDs.Select(x => new C4String(x)).ToArray();
            var sliceArr = c4StringArr.Select(x => x.AsC4Slice()).ToArray();
            var retVal = default(C4DocEnumerator*);
            fixed(C4Slice* ptr = sliceArr) {
                retVal = _c4db_enumerateSomeDocs(db, ptr, (UIntPtr)(uint)docIDs.Length, options, outError);
                #if DEBUG && !NET_3_5
                if(retVal != null) {
                    _AllocatedObjects.TryAdd((IntPtr)retVal, "C4DocEnumerator");
                #if ENABLE_LOGGING
                    Console.WriteLine("[c4db_enumerateSomeDocs] Allocated 0x{0}", ((IntPtr)retVal).ToString("X"));
                #endif
                }
                #endif
            }

            foreach (var c4str in c4StringArr) {
                c4str.Dispose();
            }

            return retVal;
        }

        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi, EntryPoint="c4enum_nextDocument")]
        private static extern C4Document* _c4enum_nextDocument(C4DocEnumerator *e, C4Error *outError);

        /// <summary>
        /// Returns the next document from an enumerator, or NULL if there are no more.
        /// The caller is responsible for freeing the C4Document.
        /// Don't forget to free the enumerator itself when finished with it.
        /// </summary>
        /// <param name="e">The enumerator to operate on</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>A pointer to the document on success, otherwise null</returns>
        public static C4Document* c4enum_nextDocument(C4DocEnumerator *e, C4Error *outError)
        {
            #if DEBUG && !NET_3_5
            var retVal = _c4enum_nextDocument(e, outError);
            if(retVal != null) {
                _AllocatedObjects.TryAdd((IntPtr)retVal, "C4Document");
                #if ENABLE_LOGGING
                Console.WriteLine("[c4enum_nextDocument] Allocated 0x{0}", ((IntPtr)retVal).ToString("X"));
                #endif
            }

            return retVal;
            #else
            return _c4enum_nextDocument(e, outError);
            #endif
        }

        /// <summary>
        /// Adds a revision to a document, as a child of the currently selected revision
        /// (or as a root revision if there is no selected revision.)
        /// On success, the new revision will be selected.
        /// Must be called within a transaction.
        /// </summary>
        /// <param name="doc">The document to operate on</param>
        /// <param name="revID">The ID of the revision being inserted</param>
        /// <param name="body">The (JSON) body of the revision</param>
        /// <param name="deleted">True if this revision is a deletion (tombstone)</param>
        /// <param name="hasAttachments">True if this revision contains an _attachments dictionary</param>
        /// <param name="allowConflict">If false, and the parent is not a leaf, a 409 error is returned</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>The number of revisions inserted (0, 1, or -1 on error)</returns>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        public static extern int c4doc_insertRevision(C4Document *doc, C4Slice revID, C4Slice body, 
            [MarshalAs(UnmanagedType.U1)]bool deleted, [MarshalAs(UnmanagedType.U1)]bool hasAttachments,
            [MarshalAs(UnmanagedType.U1)]bool allowConflict, C4Error *outError);
        
        /// <summary>
        /// Adds a revision to a document, as a child of the currently selected revision
        /// (or as a root revision if there is no selected revision.)
        /// On success, the new revision will be selected.
        /// Must be called within a transaction.
        /// </summary>
        /// <param name="doc">The document to operate on</param>
        /// <param name="revID">The ID of the revision being inserted</param>
        /// <param name="body">The (JSON) body of the revision</param>
        /// <param name="deleted">True if this revision is a deletion (tombstone)</param>
        /// <param name="hasAttachments">True if this revision contains an _attachments dictionary</param>
        /// <param name="allowConflict">If false, and the parent is not a leaf, a 409 error is returned</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>The number of revisions inserted (0, 1, or -1 on error)</returns>
        public static int c4doc_insertRevision(C4Document *doc, string revID, string body, bool deleted, bool hasAttachments,
            bool allowConflict, C4Error *outError)
        {
            using(var revID_ = new C4String(revID))
            using(var body_ = new C4String(body)) {
                return c4doc_insertRevision(doc, revID_.AsC4Slice(), body_.AsC4Slice(), deleted, 
                    hasAttachments, allowConflict, outError);
            }
        }

        /// <summary>
        ///  Adds a revision to a document, plus its ancestors (given in reverse chronological order.)
        /// On success, the new revision will be selected.
        /// Must be called within a transaction.
        /// <param name="doc">The document to operate on</param>
        /// <param name="body">The (JSON) body of the revision</param>
        /// <param name="deleted">True if this revision is a deletion (tombstone)</param>
        /// <param name="hasAttachments">True if this revision contains an _attachments dictionary</param>
        /// <param name="history">The ancestors' revision IDs, starting with the parent, in reverse order</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>The number of revisions added to the document, or -1 on error.</returns>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        public static extern int c4doc_insertRevisionWithHistory(C4Document *doc, C4Slice body, 
            [MarshalAs(UnmanagedType.U1)]bool deleted, [MarshalAs(UnmanagedType.U1)]bool hasAttachments, C4Slice* history, 
            uint historyCount, C4Error *outError);

        /// <summary>
        ///  Adds a revision to a document, plus its ancestors (given in reverse chronological order.)
        /// On success, the new revision will be selected.
        /// Must be called within a transaction.
        /// <param name="doc">The document to operate on</param>
        /// <param name="body">The (JSON) body of the revision</param>
        /// <param name="deleted">True if this revision is a deletion (tombstone)</param>
        /// <param name="hasAttachments">True if this revision contains an _attachments dictionary</param>
        /// <param name="history">The ancestors' revision IDs, starting with the parent, in reverse order</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>The number of revisions added to the document, or -1 on error.</returns>
        public static int c4doc_insertRevisionWithHistory(C4Document *doc, string body, bool deleted, 
            bool hasAttachments, string[] history, C4Error *outError)
        {
            var flattenedStringArray = new C4String[history.Length + 1];
            flattenedStringArray[0] = new C4String(body);
            for(int i = 0; i < history.Length; i++) {
                flattenedStringArray[i + 1] = new C4String(history[i]);
            }
            
            var sliceArray = flattenedStringArray.Skip(1).Select<C4String, C4Slice>(x => x.AsC4Slice()).ToArray(); 
            var retVal = default(int);
            fixed(C4Slice *a = sliceArray) {
                retVal = c4doc_insertRevisionWithHistory(doc, 
                    flattenedStringArray[0].AsC4Slice(), deleted, hasAttachments, a, (uint)history.Length, outError);
            }
            
            foreach(var s in flattenedStringArray) {
                s.Dispose();   
            }
            
            return retVal;
        }

        /// <summary>
        /// Sets the document type on a given document
        /// </summary>
        /// <returns><c>true</c>, if the operation succeeeded, <c>false</c> otherwise.</returns>
        /// <param name="doc">The document to modify.</param>
        /// <param name="docType">The type to set.</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4doc_setType(C4Document *doc, C4Slice docType, C4Error *outError);

        /// <summary>
        /// Sets a document's docType. (By convention this is the value of the "type" property of the 
        /// current revision's JSON; this value can be used as optimization when indexing a view.)
        /// The change will not be persisted until the document is saved.
        /// </summary>
        /// <param name="doc">The document to operate on</param>
        /// <param name="docType">The document type to set</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>true on success, false otherwise</returns>
        public static bool c4doc_setType(C4Document *doc, string docType, C4Error *outError)
        {
            using(var docType_ = new C4String(docType)) {
                return c4doc_setType(doc, docType_.AsC4Slice(), outError);   
            }
        }

        /// <summary>
        /// Purges a revision from a document.  As with purgeDoc, this operation
        /// is immediate and non-replicating.
        /// </summary>
        /// <returns>1 if the revision was purged, 0 if the revision didn't exist,
        /// or -1 on error</returns>
        /// <param name="doc">The document to modify.</param>
        /// <param name="revId">The ID of the revision to purge</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        public static extern int c4doc_purgeRevision(C4Document *doc, C4Slice revId, C4Error *outError);

        /// <summary>
        /// Purges a revision from a document.  As with purgeDoc, this operation
        /// is immediate and non-replicating.
        /// </summary>
        /// <returns>1 if the revision was purged, 0 if the revision didn't exist,
        /// or -1 on error</returns>
        /// <param name="doc">The document to modify.</param>
        /// <param name="revId">The ID of the revision to purge</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        public static int c4doc_purgeRevision(C4Document *doc, string revId, C4Error *outError)
        {
            using(var revId_ = new C4String(revId)) {
                return c4doc_purgeRevision(doc, revId_.AsC4Slice(), outError);   
            }
        }
            
        /// <summary>
        /// Saves changes to a C4Document.
        /// Must be called within a transaction.
        /// The revision history will be pruned to the maximum depth given.
        /// </summary>
        /// <param name="doc">The document to operate on</param>
        /// <param name="maxRevTreeDepth">The maximum number of revision history to save</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>true on success, false otherwise</returns>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4doc_save(C4Document *doc, uint maxRevTreeDepth, C4Error *outError);

        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi, EntryPoint="c4key_new")]
        private static extern C4Key* _c4key_new();

        /// <summary>
        /// Creates a new empty C4Key.
        /// </summary>
        /// <returns>A pointer to a new empty C4Key</returns>
        public static C4Key* c4key_new()
        {
            #if DEBUG && !NET_3_5
            var retVal = _c4key_new();
            if(retVal != null) {
                _AllocatedObjects.TryAdd((IntPtr)retVal, "C4Key");
                #if ENABLE_LOGGING
                Console.WriteLine("[c4key_new] Allocated 0x{0}", ((IntPtr)retVal).ToString("X"));
                #endif
            }

            return retVal;
            #else
            return _c4key_new();
            #endif
        }
        
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi, EntryPoint="c4key_withBytes")]
        private static extern C4Key* _c4key_withBytes(C4Slice slice);

        /// <summary>
        /// Creates a C4Key by copying the data, which must be in the C4Key binary format.
        /// </summary>
        /// <param name="bytes">The data to use in the C4Key</param>
        /// <returns>A pointer to the created C4Key</returns>
        public static C4Key* c4key_withBytes(C4Slice slice)
        {
            #if DEBUG && !NET_3_5
            var retVal = _c4key_withBytes(slice);
            if(retVal != null) {
                _AllocatedObjects.TryAdd((IntPtr)retVal, "C4Key");
                #if ENABLE_LOGGING
                Console.WriteLine("[c4key_withBytes] Allocated 0x{0}", ((IntPtr)retVal).ToString("X"));
                #endif
            }

            return retVal;
            #else
            return _c4key_withBytes(slice);
            #endif
        }

        /// <summary>
        /// Creates a C4Key by copying the data, which must be in the C4Key binary format.
        /// </summary>
        /// <param name="bytes">The data to use in the C4Key</param>
        /// <returns>A pointer to the created C4Key</returns>
        public static C4Key* c4key_withBytes(IEnumerable<byte> bytes)
        {
            var realized = bytes.ToArray();
            fixed(byte* ptr = realized) {
                var slice = new C4Slice(ptr, (uint)realized.Length);
                return c4key_withBytes(slice);
            }
        }

        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi, EntryPoint="c4key_free")]
        private static extern void _c4key_free(C4Key *key);

        /// <summary>
        /// Frees a C4Key.
        /// </summary>
        /// <param name="key">The key to free</param>
        public static void c4key_free(C4Key *key)
        {
            #if DEBUG && !NET_3_5
            var ptr = (IntPtr)key;
            #if ENABLE_LOGGING
            if(ptr != IntPtr.Zero && !_AllocatedObjects.ContainsKey(ptr)) {
                Console.WriteLine("WARNING: [c4key_free] freeing object 0x{0} that was not found in allocated list", ptr.ToString("X"));
            } else {
            #endif
                _AllocatedObjects.TryRemove(ptr, out _Dummy);
            #if ENABLE_LOGGING
            }
            #endif
            #endif
            _c4key_free(key);
        }

        /// <summary>
        /// Adds a JSON null value to a C4Key.
        /// </summary>
        /// <param name="key">The key to operate on</param>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        public static extern void c4key_addNull(C4Key *key);

        /// <summary>
        /// Adds a boolean value to a C4Key.
        /// </summary>
        /// <param name="key">he key to operate on</param>
        /// <param name="b">The value to store</param>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        public static extern void c4key_addBool(C4Key *key, [MarshalAs(UnmanagedType.U1)]bool b);

        /// <summary>
        /// Adds a number to a C4Key.
        /// </summary>
        /// <param name="key">The key to operate on</param>
        /// <param name="d">The value to store</param>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        public static extern void c4key_addNumber(C4Key *key, double d);

        /// <summary>
        /// Adds a string to a C4Key.
        /// </summary>
        /// <param name="key"The key to operate on></param>
        /// <param name="s">The value to store</param>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        public static extern void c4key_addString(C4Key *key, C4Slice s);

        /// <summary>
        /// Adds a string to a C4Key.
        /// </summary>
        /// <param name="key"The key to operate on></param>
        /// <param name="s">The value to store</param>
        public static void c4key_addString(C4Key *key, string s)
        {
            using(var s_ = new C4String(s)) {
                c4key_addString(key, s_.AsC4Slice());   
            }
        }

        /// <summary>
        /// Adds a map key, before the next value. When adding to a map, every value must be
        /// preceded by a key.
        /// </summary>
        /// <param name="key">The key to operate on</param>
        /// <param name="s">The value to store</param>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        public static extern void c4key_addMapKey(C4Key *key, C4Slice s);

        /// <summary>
        /// Adds a map key, before the next value. When adding to a map, every value must be
        /// preceded by a key.
        /// </summary>
        /// <param name="key">The key to operate on</param>
        /// <param name="s">The value to store</param>
        public static void c4key_addMapKey(C4Key *key, string s)
        {
            using(var s_ = new C4String(s)) {
                c4key_addMapKey(key, s_.AsC4Slice());   
            }
        }

        /// <summary>
        /// Adds an array to a C4Key.
        /// Subsequent values added will go into the array, until c4key_endArray is called.
        /// </summary>
        /// <param name="key">The key to operate on</param>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        public static extern void c4key_beginArray(C4Key *key);

        /// <summary>
        /// Closes an array opened by c4key_beginArray. (Every array must be closed.)
        /// </summary>
        /// <param name="key">The key to operate on</param>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        public static extern void c4key_endArray(C4Key *key);

        /// <summary>
        /// Adds a map/dictionary/object to a C4Key.
        /// Subsequent keys and values added will go into the map, until c4key_endMap is called.
        /// </summary>
        /// <param name="key">The key to operate on</param>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        public static extern void c4key_beginMap(C4Key *key);

        /// <summary>
        /// Closes a map opened by c4key_beginMap. (Every map must be closed.)
        /// </summary>
        /// <param name="key">The key to operate on</param>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        public static extern void c4key_endMap(C4Key *key);

        /// <summary>
        /// Returns a C4KeyReader that can parse the contents of a C4Key.
        /// Warning: Adding to the C4Key will invalidate the reader.
        /// </summary>
        /// <param name="key">The key to operate on</param>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        public static extern C4KeyReader c4key_read(C4Key *key);

        /// <summary>
        /// Returns the type of the next item in the key, or kC4Error at the end of the key or if the
        /// data is corrupt.
        /// To move on to the next item, you must call skipToken or one of the read___ functions.
        /// </summary>
        /// <param name="reader">The reader to operate on</param>
        /// <returns>The type of the next item in the key</returns>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        public static extern C4KeyToken c4key_peek(C4KeyReader *reader);

        /// <summary>
        /// Skips the current token in the key. If it was kC4Array or kC4Map, the reader will
        /// now be positioned at the first item of the collection.
        /// </summary>
        /// <param name="reader">The reader to operate on</param>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        public static extern void c4key_skipToken(C4KeyReader *reader);

        /// <summary>
        /// Reads a boolean value.
        /// </summary>
        /// <param name="reader">The reader to operate on</param>
        /// <returns>The boolean value of the next token of the key</returns>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4key_readBool(C4KeyReader *reader);

        /// <summary>
        /// Reads a number value
        /// </summary>
        /// <param name="reader">The reader to operate on</param>
        /// <returns>The numerical value of the next token of the key</returns>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        public static extern double c4key_readNumber(C4KeyReader *reader);

        /// <summary>
        /// Reads a string value
        /// </summary>
        /// <param name="reader">The reader to operate on</param>
        /// <returns>The string value of the next token of the key</returns>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi, EntryPoint="c4key_readString")]
        public static extern C4Slice _c4key_readString(C4KeyReader *reader);

        /// <summary>
        /// Reads a string value
        /// </summary>
        /// <param name="reader">The reader to operate on</param>
        /// <returns>The string value of the next token of the key</returns>
        public static string c4key_readString(C4KeyReader *reader)
        {
            return BridgeSlice(() => _c4key_readString(reader));
        }

        /// <summary>
        /// Converts a C4KeyReader to JSON.
        /// </summary>
        /// <param name="reader">The reader to operate on</param>
        /// <returns>The JSON string result</returns>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi, EntryPoint="c4key_toJSON")]
        public static extern C4Slice _c4key_toJSON(C4KeyReader *reader);

        /// <summary>
        /// Converts a C4KeyReader to JSON.
        /// </summary>
        /// <param name="reader">The reader to operate on</param>
        /// <returns>The JSON string result</returns>
        public static string c4key_toJSON(C4KeyReader *reader)
        {
            return BridgeSlice(() => _c4key_toJSON(reader));
        }
        
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi, EntryPoint="c4view_open")]
        private static extern C4View* _c4view_open(C4Database *db, C4Slice path, C4Slice viewName, C4Slice version, 
            C4DatabaseFlags flags, C4EncryptionKey *encryptionKey, C4Error *outError);

        /// <summary>
        /// Opens a view, or creates it if the file doesn't already exist.
        /// </summary>
        /// <param name="db">The database the view is associated with</param>
        /// <param name="path">The file that the view is stored in</param>
        /// <param name="viewName">The name of the view</param>
        /// <param name="version">The version of the view's map function</param>
        /// <param name="flags">The flags for opening the view file</param>
        /// <param name="encryptionKey">The option encryption key used to encrypt the database</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>A pointer to the view on success, otherwise null</returns>
        public static C4View* c4view_open(C4Database *db, C4Slice path, C4Slice viewName, C4Slice version, 
            C4DatabaseFlags flags, C4EncryptionKey *encryptionKey, C4Error *outError)
        {
            #if DEBUG && !NET_3_5
            var retVal = _c4view_open(db, path, viewName, version, flags, encryptionKey, outError);
            if(retVal != null) {
                _AllocatedObjects.TryAdd((IntPtr)retVal, "C4View");
                #if ENABLE_LOGGING
                Console.WriteLine("[c4view_open] Allocated 0x{0}", ((IntPtr)retVal).ToString("X"));
                #endif
            }

            return retVal;
            #else
            return _c4view_open(db, path, viewName, version, flags, encryptionKey, outError);
            #endif
        }

        /// <summary>
        /// Opens a view, or creates it if the file doesn't already exist.
        /// </summary>
        /// <param name="db">The database the view is associated with</param>
        /// <param name="path">The file that the view is stored in</param>
        /// <param name="viewName">The name of the view</param>
        /// <param name="version">The version of the view's map function</param>
        /// <param name="flags">The flags for opening the view file</param>
        /// <param name="encryptionKey">The option encryption key used to encrypt the database</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>A pointer to the view on success, otherwise null</returns>
        public static C4View* c4view_open(C4Database *db, string path, string viewName, string version, C4DatabaseFlags flags,
            C4EncryptionKey *encryptionKey, C4Error *outError)
        {
            using(var path_ = new C4String(path))
            using(var viewName_ = new C4String(viewName))
            using(var version_ = new C4String(version)) {
                return c4view_open(db, path_.AsC4Slice(), viewName_.AsC4Slice(), version_.AsC4Slice(), flags, encryptionKey, outError);   
            }
        }

        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi, EntryPoint="c4view_close")]
        [return: MarshalAs(UnmanagedType.U1)]
        private static extern bool _c4view_close(C4View *view, C4Error *outError);

        /// <summary>
        /// Closes the view and frees the object.
        /// </summary>
        /// <param name="view">The view to close</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>true on success, false otherwise</returns>
        public static bool c4view_close(C4View *view, C4Error *outError)
        {
            #if DEBUG && !NET_3_5
            var ptr = (IntPtr)view;
            #if ENABLE_LOGGING
            if(ptr != IntPtr.Zero && !_AllocatedObjects.ContainsKey(ptr)) {
                Console.WriteLine("WARNING: [c4view_close] freeing object 0x{0} that was not found in allocated list", ptr.ToString("X"));
            } else {
            #endif
                _AllocatedObjects.TryRemove(ptr, out _Dummy);
            #if ENABLE_LOGGING
            }
            #endif
            #endif
            return _c4view_close(view, outError);
        }

        /// <summary>
        /// Erases the view index, but doesn't delete the database file.
        /// </summary>
        /// <param name="view">The view that will have its index erased</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>true on success, false otherwise</returns>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4view_eraseIndex(C4View *view, C4Error *outError);

        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi, EntryPoint="c4view_delete")]
        [return: MarshalAs(UnmanagedType.U1)]
        private static extern bool _c4view_delete(C4View *view, C4Error *outError);

        /// <summary>
        /// Deletes the database file and closes/frees the C4View.
        /// </summary>
        /// <param name="view">The view to operate on</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>true on success, false otherwise</returns>
        public static bool c4view_delete(C4View *view, C4Error *outError)
        {
            #if DEBUG && !NET_3_5
            var ptr = (IntPtr)view;
            #if ENABLE_LOGGING
            if(ptr != IntPtr.Zero && !_AllocatedObjects.ContainsKey(ptr)) {
                Console.WriteLine("WARNING: [c4view_delete] freeing object 0x{0} that was not found in allocated list", ptr.ToString("X"));
            } else {
            #endif
                _AllocatedObjects.TryRemove(ptr, out _Dummy);
            #if ENABLE_LOGGING
            }
            #endif
            #endif
            return _c4view_delete(view, outError);
        }

        /// <summary>
        /// Returns the total number of rows in the view index.
        /// </summary>
        /// <param name="view">The view to operate on</param>
        /// <returns>The total number of rows in the view index</returns>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        public static extern ulong c4view_getTotalRows(C4View *view);

        /// <summary>
        /// Returns the last database sequence number that's been indexed.
        /// If this is less than the database's lastSequence, the view index is out of date.
        /// </summary>
        /// <param name="view">The view to operate on</param>
        /// <returns>The last database sequence number that's been indexed</returns>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        public static extern ulong c4view_getLastSequenceIndexed(C4View *view);

        /// <summary>
        /// Returns the last database sequence number that changed the view index.
        /// </summary>
        /// <param name="view">The view to operate on</param>
        /// <returns>The last database sequence number that changed the view index</returns>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        public static extern ulong c4view_getLastSequenceChangedAt(C4View *view);

        /// <summary>
        /// Changes the encryption key on a given view
        /// </summary>
        /// <returns><c>true</c>, if the operation succeeded, <c>false</c> otherwise.</returns>
        /// <param name="view">The view to change the encryption key of.</param>
        /// <param name="newKey">The new encryption key to use</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4view_rekey(C4View *view, C4EncryptionKey *newKey, C4Error *outError);

        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi, EntryPoint="c4indexer_begin")]
        private static extern C4Indexer* _c4indexer_begin(C4Database *db, C4View** views, UIntPtr viewCount, C4Error *outError);

        /// <summary>
        /// Creates an indexing task on one or more views in a database.
        /// </summary>
        /// <param name="db">The database to operate on</param>
        /// <param name="views">An array of views whose indexes should be updated in parallel.</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>A pointer to the indexer on success, otherwise null</returns>
        public static C4Indexer* c4indexer_begin(C4Database* db, C4View*[] views, C4Error* outError)
        {
            fixed(C4View** viewPtr = views) {
                #if DEBUG && !NET_3_5
                var retVal = _c4indexer_begin(db, viewPtr, (UIntPtr)(uint)views.Length, outError);
                if(retVal != null) {
                    _AllocatedObjects.TryAdd((IntPtr)retVal, "C4Indexer");
                #if ENABLE_LOGGING
                    Console.WriteLine("[c4indexer_begin] Allocated 0x{0}", ((IntPtr)retVal).ToString("X"));
                #endif
                }

                return retVal;
                #else
                return _c4indexer_begin(db, viewPtr, (UIntPtr)(uint)views.Length, outError);
                #endif
            }
        }
            
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi, EntryPoint="c4indexer_enumerateDocuments")]
        private static extern C4DocEnumerator* _c4indexer_enumerateDocuments(C4Indexer *indexer, C4Error *outError);

        /// <summary>
        /// Enumerate the documents that still need to be indexed in a given indexer
        /// </summary>
        /// <returns>The enumerator, or null on failure</returns>
        /// <param name="indexer">The indexer to check for documents.</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        public static C4DocEnumerator *c4indexer_enumerateDocuments(C4Indexer *indexer, C4Error *outError)
        {
            #if DEBUG && !NET_3_5
            var retVal = _c4indexer_enumerateDocuments(indexer, outError);
            if(retVal != null) {
                _AllocatedObjects.TryAdd((IntPtr)retVal, "C4DocEnumerator");
                #if ENABLE_LOGGING
                Console.WriteLine("[c4indexer_enumerateDocuments] Allocated 0x{0}", ((IntPtr)retVal).ToString("X"));
                #endif
            }

            return retVal;
            #else
            return _c4indexer_enumerateDocuments(indexer, outError);
            #endif
        }

        /// <summary>
        /// Creates an enumerator that will return all the documents that need to be (re)indexed.
        /// </summary>
        /// <param name="indexer">The indexer to operate on</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>A pointer to the enumerator on success, otherwise null</returns>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4indexer_emit(C4Indexer *indexer, C4Document *document, uint viewNumber, uint emitCount,
            C4Key** emittedKeys, C4Slice* emittedValues, C4Error *outError);

        /// <summary>
        /// Emits new keys/values derived from one document, for one view.
        /// This function needs to be called once for each(document, view) pair.Even if the view's map
        /// function didn't emit anything, the old keys/values need to be cleaned up.
        ///      
        /// Values are uninterpreted by CBForest, but by convention are JSON. A special value "*"
        /// (a single asterisk) is used as a placeholder for the entire document.
        ///
        /// </summary>
        /// <param name="indexer">The indexer to operate on</param>
        /// <param name="document">The document being indexed</param>
        /// <param name="viewNumber">The position of the view in the indexer's views[] array</param>
        /// <param name="emittedKeys">Array of keys being emitted</param>
        /// <param name="emittedValues">Array of values being emitted. (JSON by convention.)</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>true on success, false otherwise</returns>
        public static bool c4indexer_emit(C4Indexer *indexer, C4Document *document, uint viewNumber,
            C4Key*[] emittedKeys, C4Slice[] emittedValues, C4Error *outError)
        {
            fixed(C4Key** keysPtr = emittedKeys)
            fixed(C4Slice* valuesPtr = emittedValues)
            {
                return c4indexer_emit(indexer, document, viewNumber, (uint)emittedKeys.Length, keysPtr, valuesPtr, outError);
            }
        }

        /// <summary>
        /// Emits new keys/values derived from one document, for one view.
        /// This function needs to be called once for each(document, view) pair.Even if the view's map
        /// function didn't emit anything, the old keys/values need to be cleaned up.
        ///      
        /// Values are uninterpreted by CBForest, but by convention are JSON. A special value "*"
        /// (a single asterisk) is used as a placeholder for the entire document.
        ///
        /// </summary>
        /// <param name="indexer">The indexer to operate on</param>
        /// <param name="document">The document being indexed</param>
        /// <param name="viewNumber">The position of the view in the indexer's views[] array</param>
        /// <param name="emittedKeys">Array of keys being emitted</param>
        /// <param name="emittedValues">Array of values being emitted. (JSON by convention.)</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>true on success, false otherwise</returns>
        public static bool c4indexer_emit(C4Indexer *indexer, C4Document *document, uint viewNumber,
            C4Key*[] emittedKeys, string[] emittedValues, C4Error *outError)
        {
            var c4StringArr = emittedValues.Select(x => new C4String(x)).ToArray();
            var sliceArr = c4StringArr.Select(x => x.AsC4Slice()).ToArray();
            var retVal = c4indexer_emit(indexer, document, viewNumber, emittedKeys, sliceArr, outError);
            foreach(var c4str in c4StringArr) {
                c4str.Dispose();
            }
    
            return retVal;
        }

        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi, EntryPoint="c4indexer_end")]
        [return: MarshalAs(UnmanagedType.U1)]
        private static extern bool _c4indexer_end(C4Indexer *indexer, [MarshalAs(UnmanagedType.U1)]bool commit, C4Error *outError);

        /// <summary>
        /// Finishes an indexing task and frees the indexer reference.
        /// </summary>
        /// <param name="indexer">The indexer to operate on</param>
        /// <param name="commit">True to commit changes to the indexes, false to abort</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>true on success, false otherwise</returns>
        public static bool c4indexer_end(C4Indexer *indexer, bool commit, C4Error *outError)
        {
            #if DEBUG && !NET_3_5
            var ptr = (IntPtr)indexer;
            #if ENABLE_LOGGING
            if(ptr != IntPtr.Zero && !_AllocatedObjects.ContainsKey(ptr)) {
                Console.WriteLine("WARNING: [c4indexer_end] freeing object 0x{0} that was not found in allocated list", ptr.ToString("X"));
            } else {
            #endif
                _AllocatedObjects.TryRemove(ptr, out _Dummy);
            #if ENABLE_LOGGING
            }
            #endif
            #endif
            return _c4indexer_end(indexer, commit, outError);
        }

        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi, EntryPoint="c4view_query")]
        private static extern C4QueryEnumerator* _c4view_query(C4View *view, C4QueryOptions *options, C4Error *outError);

        /// <summary>
        /// Runs a query and returns an enumerator for the results.
        /// The enumerator's fields are not valid until you call c4queryenum_next(), though.
        /// </summary>
        /// <param name="view">The view to operate on</param>
        /// <param name="options">The query options</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>A pointer to the enumerator on success, otherwise null</returns>
        public static C4QueryEnumerator *c4view_query(C4View *view, C4QueryOptions *options, C4Error *outError)
        {
            #if DEBUG && !NET_3_5
            var retVal = _c4view_query(view, options, outError);
            if(retVal != null) {
                _AllocatedObjects.TryAdd((IntPtr)retVal, "C4QueryEnumerator");
                #if ENABLE_LOGGING
                Console.WriteLine("[c4view_query] Allocated 0x{0}", ((IntPtr)retVal).ToString("X"));
                #endif
            }

            return retVal;
            #else
            return _c4view_query(view, options, outError);
            #endif
        }

        /// <summary>
        /// Advances a query enumerator to the next row, populating its fields.
        /// Returns true on success, false at the end of enumeration or on error.
        /// </summary>
        /// <param name="e">The enumerator to operate on</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>true on success, false on error or end reached</returns>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4queryenum_next(C4QueryEnumerator *e, C4Error *outError);

        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi, EntryPoint="c4queryenum_free")]
        private static extern void _c4queryenum_free(C4QueryEnumerator *e);

        /// <summary>
        /// Frees a query enumerator.
        /// </summary>
        /// <param name="e">The enumerator to free</param>
        public static void c4queryenum_free(C4QueryEnumerator *e)
        {
            #if DEBUG && !NET_3_5
            var ptr = (IntPtr)e;
            #if ENABLE_LOGGING
            if(ptr != IntPtr.Zero && !_AllocatedObjects.ContainsKey(ptr)) {
                Console.WriteLine("WARNING: [c4queryenum_free] freeing object 0x{0} that was not found in allocated list", ptr.ToString("X"));
            } else {
            #endif
                _AllocatedObjects.TryRemove(ptr, out _Dummy);
            #if ENABLE_LOGGING
            }
            #endif
            #endif
            _c4queryenum_free(e);
        }
        
        /// <summary>
        /// Registers (or unregisters) a log callback, and sets the minimum log level to report.
        /// Before this is called, logs are by default written to stderr for warnings and errors.
        ///    Note that this setting is global to the entire process.
        /// </summary>
        /// <param name="level">The minimum level of message to log</param>
        /// <param name="callback">The logging callback, or NULL to disable logging entirely</param>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        private static extern void c4log_register(C4LogLevel level, C4LogCallback callback);

        /// <summary>
        /// Register a logging callback with CBForest.  All internal logs will be forwarded
        /// to this callback.
        /// </summary>
        /// <param name="level">The logging level to debug (the specified level and higher severity).</param>
        /// <param name="callback">The logging callback logic.</param>
        public static void c4log_register(C4LogLevel level, Action<C4LogLevel, string> callback)
        {
            _LogCallback = callback;

            // This is needed to ensure that the delegate object itself doesn't get garbage collected
            _NativeLogCallback = _LogCallback == null ? null : new C4LogCallback(c4log_wedge);
            c4log_register(level, _NativeLogCallback);
        }
        
        private static string BridgeSlice(Func<C4Slice> nativeFunc)
        {
            var rawRetVal = nativeFunc();
            var retVal = (string)rawRetVal;
            c4slice_free(rawRetVal);
            return retVal;
        }

        #if __IOS__
        [ObjCRuntime.MonoPInvokeCallback(typeof(C4LogCallback))]
        #endif
        private static void c4log_wedge(C4LogLevel level, C4Slice message)
        {
            if (_LogCallback != null) {
                var sharpString = (string)message;
                _LogCallback(level, sharpString);
            }
        }
    }
}

