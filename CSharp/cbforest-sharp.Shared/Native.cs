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
//#define TRACE
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Linq;
using System.Runtime.InteropServices;
using System.IO;

#if __IOS__ && !FAKE
[assembly: ObjCRuntime.LinkWith("libCBForest-Interop.a", 
    ObjCRuntime.LinkTarget.Arm64 | ObjCRuntime.LinkTarget.ArmV7 | ObjCRuntime.LinkTarget.ArmV7s, ForceLoad=true,
    LinkerFlags="-lsqlite3 -lc++", Frameworks="Security", IsCxx=true)]
#endif

namespace CBForest
{
    /// <summary>
    /// Bridge into CBForest-Interop.dll
    /// </summary>
#if __IOS__
    [Foundation.Preserve(AllMembers = true)]
#endif
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
            { "C4Database", p => _c4db_free((C4Database*)p.ToPointer()) },
            { "C4RawDocument", p => _c4raw_free((C4RawDocument*)p.ToPointer()) },
            { "C4Document", p => _c4doc_free((C4Document*)p.ToPointer()) },
            { "C4DocEnumerator", p => _c4enum_free((C4DocEnumerator*)p.ToPointer()) },
            { "C4Key", p => _c4key_free((C4Key*)p.ToPointer()) },
            { "C4View", p => _c4view_free((C4View*)p.ToPointer()) },
            { "C4Indexer", p => _c4indexer_end((C4Indexer*)p.ToPointer(), false, null) },
            { "C4QueryEnumerator", p => _c4queryenum_free((C4QueryEnumerator*)p.ToPointer()) },
            { "C4KeyValueList", p => _c4kv_free((C4KeyValueList*)p.ToPointer()) }
        };
#endif

        internal static Action<C4LogLevel, string> _LogCallback;
        private static C4LogCallback _NativeLogCallback;

        static Native()
        {
            if(Environment.OSVersion.Platform == PlatformID.Win32NT) {
                var dllName = string.Format("{0}.dll", DLL_NAME);
                var directory = AppDomain.CurrentDomain.BaseDirectory;
                if(directory == null) {
                    var codeBase = typeof(Native).Assembly.CodeBase;
                    UriBuilder uri = new UriBuilder(codeBase);
                    directory = Path.GetDirectoryName(Uri.UnescapeDataString(uri.Path));
                }

                Debug.Assert(Path.IsPathRooted(directory), "directory is not rooted.");
                var architecture = IntPtr.Size == 4
                    ? "x86"
                    : "x64";

                var dllPath = Path.Combine(Path.Combine(directory, architecture), dllName);
                if(!File.Exists(dllPath)) {
                    return;
                }

                const uint LOAD_WITH_ALTERED_SEARCH_PATH = 8;
                var ptr = LoadLibraryEx(dllPath, IntPtr.Zero, LOAD_WITH_ALTERED_SEARCH_PATH);
            }
        }

        /// <summary>
        /// Checks the internal object tracking for any objects that were not freed
        /// </summary>
        [Conditional("DEBUG")]
        public static void CheckMemoryLeaks()
        {
#if DEBUG && !NET_3_5
            foreach (var pair in _AllocatedObjects) {
                Trace.WriteLine(String.Format("ERROR: {0}* at 0x{1} leaked", pair.Value, pair.Key.ToString("X")));
                _GcAction[pair.Value](pair.Key); // To make sure the next test doesn't fail because of this one's mistakes
            }

            if(_AllocatedObjects.Count != c4_getObjectCount()) {
                throw new ApplicationException("Mismatch between allocated object map and object count");
            }

            if (_AllocatedObjects.Any()) {
                _AllocatedObjects.Clear();
                throw new ApplicationException("Memory leaks detected");
            }
#endif
        }

        /// <summary>
        /// Gets the count of live unmanaged objects in CBForest (for debugging)
        /// </summary>
        /// <returns>The get object count.</returns>
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public static extern int c4_getObjectCount();

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
#if __IOS__
        [DllImport ("libc", CallingConvention = CallingConvention.Cdecl)]
#else
        [DllImport("msvcrt.dll", CallingConvention = CallingConvention.Cdecl)]
#endif
        public static extern int memcmp(void* b1, void* b2, UIntPtr count);

        #if __IOS__
        [DllImport("libc", CallingConvention = CallingConvention.Cdecl)]
        #else
        [DllImport("msvcrt.dll", CallingConvention = CallingConvention.Cdecl)]
        #endif
        public static extern int memcpy(void* dest, void* src, UIntPtr count);

        [DllImport("kernel32")]
        private static extern IntPtr LoadLibraryEx(string lpFileName, IntPtr hFile, uint dwFlags);


        /// <summary>
        /// Returns true if two slices have equal contents.
        /// </summary>
        /// <returns>true if two slices have equal contents.</returns>
        /// <param name="a">The first slice to compare</param>
        /// <param name="b">The second slice to compare</param>
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4SliceEqual(C4Slice a, C4Slice b);

        /// <summary>
        /// Frees the memory of a C4Slice.
        /// </summary>
        /// <param name="slice">The slice that will have its memory freed</param>
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public static extern void c4slice_free(C4Slice slice);

        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi, EntryPoint = "c4db_open")]
        private static extern C4Database* _c4db_open(C4Slice path, C4DatabaseFlags flags,
            C4EncryptionKey* encryptionKey, C4Error* outError);

        /// <summary>
        /// Opens a database.
        /// </summary>
        /// <param name="path">The path to the DB file</param>
        /// <param name="flags">The flags for opening the database</param>
        /// <param name="encryptionKey">The option encryption key used to encrypt the database</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>A database instance for use in the C4 API</returns>
        public static C4Database* c4db_open(C4Slice path, C4DatabaseFlags flags,
            C4EncryptionKey* encryptionKey, C4Error* outError)
        {
#if DEBUG && !NET_3_5
            var retVal = _c4db_open(path, flags, encryptionKey, outError);
            if(retVal != null) {
                _AllocatedObjects.TryAdd((IntPtr)retVal, "C4Database");
                #if TRACE
                Trace.WriteLine("[c4db_open] Allocated 0x{0}", ((IntPtr)retVal).ToString("X"));
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
        /// <param name="flags">The flags for opening the database</param>
        /// <param name="encryptionKey">The option encryption key used to encrypt the database</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>A database instance for use in the C4 API</returns>
        public static C4Database* c4db_open(string path, C4DatabaseFlags flags,
            C4EncryptionKey* encryptionKey, C4Error* outError)
        {
            using(var path_ = new C4String(path)) {
                return c4db_open(path_.AsC4Slice(), flags, encryptionKey, outError);
            }
        }

        /// <summary>
        /// Closes a database (any further access attempts are invalid)
        /// </summary>
        /// <returns>Whether or not the database was successfully closed</returns>
        /// <param name="db">The database to close</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4db_close(C4Database* db, C4Error* outError);

        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi, EntryPoint = "c4db_free")]
        [return: MarshalAs(UnmanagedType.U1)]
        private static extern bool _c4db_free(C4Database* db);

        /// <summary>
        /// Closes the database and frees the object.
        /// </summary>
        /// <param name="db">The DB object to close</param>
        /// <returns>true on success, false otherwise</returns>
        public static bool c4db_free(C4Database* db)
        {
#if DEBUG && !NET_3_5
            var ptr = (IntPtr)db;
            #if TRACE
            if(ptr != IntPtr.Zero && !_AllocatedObjects.ContainsKey(ptr)) {
                Trace.WriteLine("WARNING: [c4db_close] freeing object 0x{0} that was not found in allocated list", ptr.ToString("X"));
            } else {
#endif
                _AllocatedObjects.TryRemove(ptr, out _Dummy);
            #if TRACE
            }
#endif
#endif
            return _c4db_free(db);
        }

        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi, EntryPoint = "c4db_delete")]
        [return: MarshalAs(UnmanagedType.U1)]
        private static extern bool _c4db_delete(C4Database* db, C4Error* outError);

        /// <summary>
        /// Closes the database, deletes the file, and frees the object
        /// </summary>
        /// <param name="db">The DB object to delete</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>true on success, false otherwise</returns>
        public static bool c4db_delete(C4Database* db, C4Error* outError)
        {
#if DEBUG && !NET_3_5
            var ptr = (IntPtr)db;
            #if TRACE
            if(ptr != IntPtr.Zero && !_AllocatedObjects.ContainsKey(ptr)) {
                Trace.WriteLine("WARNING: [c4db_delete] freeing object 0x{0} that was not found in allocated list", ptr.ToString("X"));
            } else {
#endif
                _AllocatedObjects.TryRemove(ptr, out _Dummy);
            #if TRACE
            }
#endif
#endif
            return _c4db_delete(db, outError);
        }

        /// <summary>
        /// Deletes the file(s) for the database at the given path.
        /// All C4Databases at that path should be closed first.
        /// </summary>
        /// <returns>Whether or not the operation succeeded</returns>
        /// <param name="path">The path to delete files at</param>
        /// <param name="flags">The flags to use during deletion</param>
        /// <param name="outError">Any errors that occurred will be recorded here</param> 
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4db_deleteAtPath(C4Slice path, C4DatabaseFlags flags, C4Error* outError);

        /// <summary>
        /// Deletes the file(s) for the database at the given path.
        /// All C4Databases at that path should be closed first.
        /// </summary>
        /// <returns>Whether or not the operation succeeded</returns>
        /// <param name="path">The path to delete files at</param>
        /// <param name="flags">The flags to use during deletion</param>
        /// <param name="outError">Any errors that occurred will be recorded here</param> 
        public static bool c4db_deleteAtPath(string path, C4DatabaseFlags flags, C4Error* outError)
        {
            using(var path_ = new C4String(path)) {
                return c4db_deleteAtPath(path_.AsC4Slice(), flags, outError);
            }
        }

        /// <summary>
        /// Triggers a manual compaction of the database
        /// </summary>
        /// <returns>true on success, false otherwise</returns>
        /// <param name="db">The database to compact.</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4db_compact(C4Database* db, C4Error* outError);

        /// <summary>
        /// Gets whether or not the database is currently in a compact operation
        /// </summary>
        /// <returns>Whether or not the database is currently in a compact operation</returns>
        /// <param name="db">The database to check</param>
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4db_isCompacting(C4Database* db);

        /// <summary>
        /// Changes the encryption key of a given database
        /// </summary>
        /// <returns>true on success, false otherwise</returns>
        /// <param name="db">The database to change the encryption key of</param>
        /// <param name="newKey">The new encryption key to use</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4db_rekey(C4Database* db, C4EncryptionKey* newKey, C4Error* outError);

        //TODO: setOnCompactCallback

        /// <summary>
        /// Returns the path of the database. 
        /// </summary>
        /// <returns>The path of the database</returns>
        /// <param name="db">The database to operate on</param>
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi, EntryPoint = "c4db_getPath")]
        public static extern C4Slice _c4db_getPath(C4Database* db);

        /// <summary>
        /// Returns the path of the database. 
        /// </summary>
        /// <returns>The path of the database</returns>
        /// <param name="db">The database to operate on</param>
        public static string c4db_getPath(C4Database* db)
        {
            return BridgeSlice(() => _c4db_getPath(db));
        }

        /// <summary>
        /// Returns the number of (undeleted) documents in the database.
        /// </summary>
        /// <param name="db">The database object to examine</param>
        /// <returns>The number of (undeleted) documents in the database.</returns>
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public static extern ulong c4db_getDocumentCount(C4Database* db);

        /// <summary>
        /// Returns the latest sequence number allocated to a revision.
        /// </summary>
        /// <param name="db">The database object to examine</param>
        /// <returns>The latest sequence number allocated to a revision.</returns>
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public static extern ulong c4db_getLastSequence(C4Database* db);

        /// <summary>
        /// Begins a transaction.
        /// Transactions can nest; only the first call actually creates a ForestDB transaction.
        /// </summary>
        /// <param name="db">The database object to start a transaction on</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>true on success, false otherwise</returns>
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4db_beginTransaction(C4Database* db, C4Error* outError);

        /// <summary>
        /// Commits or aborts a transaction. If there have been multiple calls to beginTransaction, 
        /// it takes the same number of calls to endTransaction to actually end the transaction; only 
        /// the last one commits or aborts the ForestDB transaction.
        /// </summary>
        /// <param name="db">The database to end a transaction on</param>
        /// <param name="commit">If true, commit the results, otherwise abort</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>true on success, false otherwise</returns>
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4db_endTransaction(C4Database* db, [MarshalAs(UnmanagedType.U1)]bool commit, C4Error* outError);

        /// <summary>
        /// Is a transaction active?
        /// </summary>
        /// <param name="db">The database to check</param>
        /// <returns>Whether or not a transaction is active</returns>
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4db_isInTransaction(C4Database* db);

        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi, EntryPoint = "c4raw_free")]
        private static extern void _c4raw_free(C4RawDocument* rawDoc);

        /// <summary>
        /// Frees the storage occupied by a raw document.
        /// </summary>
        /// <param name="rawDoc"></param>
        public static void c4raw_free(C4RawDocument* rawDoc)
        {
#if DEBUG && !NET_3_5
            var ptr = (IntPtr)rawDoc;
            #if TRACE
            if(ptr != IntPtr.Zero && !_AllocatedObjects.ContainsKey(ptr)) {
                Trace.WriteLine("WARNING: [c4db_delete] freeing object 0x{0} that was not found in allocated list", ptr.ToString("X"));
            } else {
#endif
                _AllocatedObjects.TryRemove(ptr, out _Dummy);
            #if TRACE
            }
#endif
#endif
            _c4raw_free(rawDoc);
        }

        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi, EntryPoint = "c4raw_get")]
        private static extern C4RawDocument* _c4raw_get(C4Database* db, C4Slice storeName, C4Slice docID, C4Error* outError);

        /// <summary>
        /// Reads a raw document from the database. In Couchbase Lite the store named "info" is used for 
        /// per-database key/value pairs, and the store "_local" is used for local documents.
        /// </summary>
        /// <param name="db">The database to operate on</param>
        /// <param name="storeName">The name of the store to read from</param>
        /// <param name="docID">The ID of the document to retrieve</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>A pointer to the retrieved document on success, or null on failure</returns>
        public static C4RawDocument* c4raw_get(C4Database* db, C4Slice storeName, C4Slice docID, C4Error* outError)
        {
#if DEBUG && !NET_3_5
            var retVal = _c4raw_get(db, storeName, docID, outError);
            if(retVal != null) {
                _AllocatedObjects.TryAdd((IntPtr)retVal, "C4RawDocument");
                #if TRACE
                Trace.WriteLine("[c4raw_get] Allocated 0x{0}", ((IntPtr)retVal).ToString("X"));
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
        public static C4RawDocument* c4raw_get(C4Database* db, string storeName, string docID, C4Error* outError)
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
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4raw_put(C4Database* db, C4Slice storeName, C4Slice key, C4Slice meta,
            C4Slice body, C4Error* outError);

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
        public static bool c4raw_put(C4Database* db, string storeName, string key, string meta, string body, C4Error* outError)
        {
            using(var storeName_ = new C4String(storeName))
            using(var key_ = new C4String(key))
            using(var meta_ = new C4String(meta))
            using(var body_ = new C4String(body)) {
                return c4raw_put(db, storeName_.AsC4Slice(), key_.AsC4Slice(), meta_.AsC4Slice(),
                                body_.AsC4Slice(), outError);
            }
        }

        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi, EntryPoint = "c4doc_free")]
        private static extern void _c4doc_free(C4Document* doc);

        /// <summary>
        /// Frees a C4Document.
        /// </summary>
        /// <param name="doc">The document to free</param>
        public static void c4doc_free(C4Document* doc)
        {
#if DEBUG && !NET_3_5
            var ptr = (IntPtr)doc;
            #if TRACE
            if(ptr != IntPtr.Zero && !_AllocatedObjects.ContainsKey(ptr)) {
                Trace.WriteLine("WARNING: [c4doc_free] freeing object 0x{0} that was not found in allocated list", ptr.ToString("X"));
            } else {
#endif
                _AllocatedObjects.TryRemove(ptr, out _Dummy);
            #if TRACE
            }
#endif
#endif
            _c4doc_free(doc);
        }

        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi, EntryPoint = "c4doc_get")]
        private static extern C4Document* _c4doc_get(C4Database* db, C4Slice docID, [MarshalAs(UnmanagedType.U1)]bool mustExist,
            C4Error* outError);

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
        public static C4Document* c4doc_get(C4Database* db, C4Slice docID, bool mustExist,
            C4Error* outError)
        {
#if DEBUG && !NET_3_5
            var retVal = _c4doc_get(db, docID, mustExist, outError);
            if(retVal != null) {
                _AllocatedObjects.TryAdd((IntPtr)retVal, "C4Document");
                #if TRACE
                Trace.WriteLine("[c4doc_get] Allocated 0x{0}", ((IntPtr)retVal).ToString("X"));
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
        public static C4Document* c4doc_get(C4Database* db, string docID, bool mustExist, C4Error* outError)
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
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public static extern C4Document* c4doc_getBySequence(C4Database* db, ulong sequence, C4Error* outError);

        /// <summary>
        /// Returns the document type (as set by setDocType.) This value is ignored by CBForest itself; by convention 
        /// Couchbase Lite sets it to the value of the current revision's "type" property, and uses it as an optimization 
        /// when indexing a view.
        /// </summary>
        /// <param name="doc">The document to operate on</param>
        /// <returns>The document type of the document in question</returns>
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi, EntryPoint = "c4doc_getType")]
        public static extern C4Slice _c4doc_getType(C4Document* doc);

        /// <summary>
        /// Returns the document type (as set by setDocType.) This value is ignored by CBForest itself; by convention 
        /// Couchbase Lite sets it to the value of the current revision's "type" property, and uses it as an optimization 
        /// when indexing a view.
        /// </summary>
        /// <param name="doc">The document to operate on</param>
        /// <returns>The document type of the document in question</returns>
        public static string c4doc_getType(C4Document* doc)
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
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4db_purgeDoc(C4Database* db, C4Slice docId, C4Error* outError);

        /// <summary>
        /// Purges a document from the database.  When a purge occurs a document is immediately
        /// removed with no trace.  This action is *not* replicated.
        /// </summary>
        /// <returns>true on success, false otherwise</returns>
        /// <param name="db">The database to purge the document from</param>
        /// <param name="docId">The ID of the document to purge</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        public static bool c4db_purgeDoc(C4Database* db, string docId, C4Error* outError)
        {
            using(var docId_ = new C4String(docId)) {
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
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4doc_selectRevision(C4Document* doc, C4Slice revID, [MarshalAs(UnmanagedType.U1)]bool withBody,
            C4Error* outError);

        /// <summary>
        /// Selects a specific revision of a document (or no revision, if revID is NULL.)
        /// </summary>
        /// <param name="doc">The document to operate on</param>
        /// <param name="revID">The revID of the revision to select</param>
        /// <param name="withBody">Whether or not to load the body of the revision</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>true on success, false otherwise</returns>
        public static bool c4doc_selectRevision(C4Document* doc, string revID, bool withBody, C4Error* outError)
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
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4doc_selectCurrentRevision(C4Document* doc);

        /// <summary>
        /// Populates the body field of a doc's selected revision,
        /// if it was initially loaded without its body.
        /// </summary>
        /// <param name="doc">The document to operate on</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>true on success, false otherwise</returns>
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4doc_loadRevisionBody(C4Document* doc, C4Error* outError);

        /// <summary>
        /// Checks to see if a given document has a revision body
        /// </summary>
        /// <returns><c>true</c>, if a revision body is available, <c>false</c> otherwise.</returns>
        /// <param name="doc">The document to check.</param>
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4doc_hasRevisionBody(C4Document* doc);

        /// <summary>
        /// Selects the parent of the selected revision, if it's known, else inserts null.
        /// </summary>
        /// <param name="doc">The document to operate on</param>
        /// <returns>true on success, false otherwise</returns>
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4doc_selectParentRevision(C4Document* doc);

        /// <summary>
        /// Selects the next revision in priority order.
        /// This can be used to iterate over all revisions, starting from the current revision.
        /// </summary>
        /// <param name="doc">The document to operate on</param>
        /// <returns>true on success, false otherwise</returns>
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4doc_selectNextRevision(C4Document* doc);

        /// <summary>
        /// Selects the next leaf revision; like selectNextRevision but skips over non-leaves.
        /// </summary>
        /// <param name="doc">The document to operate on</param>
        /// <param name="includeDeleted">Whether or not to include deleted leafs</param>
        /// <param name="withBody">Whether or not to automatically load the body of the revision</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>true on success, false otherwise</returns>
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4doc_selectNextLeafRevision(C4Document* doc, [MarshalAs(UnmanagedType.U1)]bool includeDeleted,
            [MarshalAs(UnmanagedType.U1)]bool withBody, C4Error* outError);

        /// <summary>
        /// Given a revision ID, returns its generation number (the decimal number before
        /// the hyphen), or zero if it's unparseable.
        /// </summary>
        /// <returns>The generation of the revision ID</returns>
        /// <param name="revID">The revision ID to inspect</param>
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public static extern int c4rev_getGeneration(C4Slice revID);

        /// <summary>
        /// Given a revision ID, returns its generation number (the decimal number before
        /// the hyphen), or zero if it's unparseable.
        /// </summary>
        /// <returns>The generation of the revision ID</returns>
        /// <param name="revID">The revision ID to inspect</param>
        public static int c4rev_getGeneration(string revID)
        {
            using(var c4str = new C4String(revID)) {
                return c4rev_getGeneration(c4str.AsC4Slice());
            }
        }

        /// <summary>
        /// Closes the given doc enumerator, all further access is invalid.
        /// </summary>
        /// <param name="e">The doc enumerator to close</param>
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public static extern void c4enum_close(C4DocEnumerator* e);

        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi, EntryPoint = "c4enum_free")]
        private static extern void _c4enum_free(C4DocEnumerator* e);

        /// <summary>
        /// Frees the resources used by a C4DocEnumerator
        /// </summary>
        /// <param name="e">The doc enumerator to free</param>
        public static void c4enum_free(C4DocEnumerator* e)
        {
#if DEBUG && !NET_3_5
            var ptr = (IntPtr)e;
            #if TRACE
            if(ptr != IntPtr.Zero && !_AllocatedObjects.ContainsKey(ptr)) {
                Trace.WriteLine("WARNING: [c4enum_free] freeing object 0x{0} that was not found in allocated list", ptr.ToString("X"));
            } else {
#endif
                _AllocatedObjects.TryRemove(ptr, out _Dummy);
            #if TRACE
            }
#endif
#endif
            _c4enum_free(e);
        }

        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi, EntryPoint = "c4db_enumerateChanges")]
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
                #if TRACE
                Trace.WriteLine("[c4db_enumerateChanges] Allocated 0x{0}", ((IntPtr)retVal).ToString("X"));
#endif
            }

            return retVal;
#else
            return _c4db_enumerateChanges(db, since, options, outError);
#endif
        }

        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi, EntryPoint = "c4db_enumerateAllDocs")]
        private static extern C4DocEnumerator* _c4db_enumerateAllDocs(C4Database* db, C4Slice startDocID, C4Slice endDocID,
            C4EnumeratorOptions* options, C4Error* outError);

        /// <summary>
        /// Creates an enumerator which will iterate over all documents in the database
        /// </summary>
        /// <returns>The enumerator object to use, or null on error</returns>
        /// <param name="db">The database to enumerate</param>
        /// <param name="startDocID">The document ID to start enumerating from</param>
        /// <param name="endDocID">The document ID to finish enumerating at</param>
        /// <param name="options">The enumeration options (null for defaults).</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        public static C4DocEnumerator* c4db_enumerateAllDocs(C4Database* db, C4Slice startDocID, C4Slice endDocID,
            C4EnumeratorOptions* options, C4Error* outError)
        {
#if DEBUG && !NET_3_5
            var retVal = _c4db_enumerateAllDocs(db, startDocID, endDocID, options, outError);
            if(retVal != null) {
                _AllocatedObjects.TryAdd((IntPtr)retVal, "C4DocEnumerator");
                #if TRACE
                Trace.WriteLine("[c4db_enumerateAllDocs] Allocated 0x{0}", ((IntPtr)retVal).ToString("X"));
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
        public static C4DocEnumerator* c4db_enumerateAllDocs(C4Database* db, string startDocID, string endDocID, C4EnumeratorOptions* options,
            C4Error* outError)
        {
            using(var startDocID_ = new C4String(startDocID))
            using(var endDocID_ = new C4String(endDocID)) {
                return c4db_enumerateAllDocs(db, startDocID_.AsC4Slice(), endDocID_.AsC4Slice(), options, outError);
            }
        }

        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi, EntryPoint = "c4db_enumerateSomeDocs")]
        private static extern C4DocEnumerator* _c4db_enumerateSomeDocs(C4Database* db, C4Slice* docIDs, UIntPtr docIDsCount,
            C4EnumeratorOptions* options, C4Error* outError);

        /// <summary>
        /// Enumerate a set of document IDs.  For each ID in the list, the enumerator
        /// will attempt to retrieve the corresponding document.
        /// </summary>
        /// <returns>The enumerator, or null on failure.</returns>
        /// <param name="db">The database to enumerate from</param>
        /// <param name="docIDs">The document IDs to enumerate</param>
        /// <param name="options">The enumeration options (null for defaults).</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        public static C4DocEnumerator* c4db_enumerateSomeDocs(C4Database* db, string[] docIDs, C4EnumeratorOptions* options,
            C4Error* outError)
        {
            var c4StringArr = docIDs.Select(x => new C4String(x)).ToArray();
            var sliceArr = c4StringArr.Select(x => x.AsC4Slice()).ToArray();
            var retVal = default(C4DocEnumerator*);
            fixed (C4Slice* ptr = sliceArr)
            {
                retVal = _c4db_enumerateSomeDocs(db, ptr, (UIntPtr)(uint)docIDs.Length, options, outError);
#if DEBUG && !NET_3_5
                if(retVal != null) {
                    _AllocatedObjects.TryAdd((IntPtr)retVal, "C4DocEnumerator");
                #if TRACE
                    Trace.WriteLine("[c4db_enumerateSomeDocs] Allocated 0x{0}", ((IntPtr)retVal).ToString("X"));
#endif
                }
#endif
            }

            foreach(var c4str in c4StringArr) {
                c4str.Dispose();
            }

            return retVal;
        }

        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi, EntryPoint="c4db_enumerateExpired")]
        private static extern C4ExpiryEnumerator *_c4db_enumerateExpired(C4Database *db, C4Error *outError);

        /// <summary>
        /// Creates an enumerator for iterating over expired documents
        /// Caller is responsible for freeing the enumerator when finished with it.
        /// </summary>
        /// <returns>The enumerate expired.</returns>
        /// <param name="db">The database.</param>
        /// <param name="outError">Error will be stored here on failure.</param>
        public static C4ExpiryEnumerator *c4db_enumerateExpired(C4Database *db, C4Error *outError)
        {
            #if DEBUG && !NET_3_5
            var retVal = _c4db_enumerateExpired(db, outError);
            if(retVal != null) {
            _AllocatedObjects.TryAdd((IntPtr)retVal, "C4ExpiryEnumerator");
            #if TRACE
            Trace.WriteLine("[c4db_enumerateExpired] Allocated 0x{0}", ((IntPtr)retVal).ToString("X"));
            #endif
            }

            return retVal;
            #else
            return _c4db_enumerateExpired(db, outError);
            #endif
        }

        /// <summary>
        /// Advances the enumerator to the next document.
        /// Returns false at the end, or on error; look at the C4Error to determine which occurred,
        /// and don't forget to free the enumerator.
        /// </summary>
        /// <returns><c>true</c> on success, <c>false</c> otherwise.</returns>
        /// <param name="e">The enumerator</param>
        /// <param name="outError">Records any error that occurred</param>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4exp_next(C4ExpiryEnumerator *e, C4Error *outError);

        /// <summary>
        /// Stores the metadata of the enumerator's current document into the supplied
        /// C4DocumentInfo struct. Unlike c4enum_getDocument(), this allocates no memory.
        /// </summary>
        /// <param name="e">The enumerator</param>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi, EntryPoint="c4exp_getDocID")]
        public static extern C4Slice _c4exp_getDocID(C4ExpiryEnumerator *e);

        /// <summary>
        /// Stores the metadata of the enumerator's current document into the supplied
        /// C4DocumentInfo struct. Unlike c4enum_getDocument(), this allocates no memory.
        /// </summary>
        /// <param name="e">The enumerator</param>
        public static string c4exp_getDocID(C4ExpiryEnumerator *e)
        {
            return BridgeSlice(() => _c4exp_getDocID(e));
        }

        /// <summary>
        /// Purges the records of expired documents from the database (doesn't purge the documents themselves,
        /// only the entires in the expiration date key store)
        /// </summary>
        /// <returns>Whether or not the operation succeeded</returns>
        /// <param name="e">The enumerator to get the entries to delete from</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4exp_purgeExpired(C4ExpiryEnumerator *e, C4Error *outError);

        /// <summary>
        /// Frees a C4DocEnumerator handle, and optionally purges the processed entries
        /// from the expiration key value store.
        /// </summary>
        /// <param name="e">The enumerator</param>
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi, EntryPoint = "c4exp_free")]
        private static extern void _c4exp_free(C4ExpiryEnumerator* e);

        /// <summary>
        /// Frees the resources from this expiration enumerator
        /// </summary>
        /// <param name="e">The enumerator to free</param>
        public static void c4exp_free(C4ExpiryEnumerator *e)
        {
#if DEBUG && !NET_3_5
            var ptr = (IntPtr)e;
#if TRACE
            if(ptr != IntPtr.Zero && !_AllocatedObjects.ContainsKey(ptr)) {
                Trace.WriteLine("WARNING: [c4exp_free] freeing object 0x{0} that was not found in allocated list", ptr.ToString("X"));
            } else {
#endif
            _AllocatedObjects.TryRemove(ptr, out _Dummy);
#if TRACE
            }
#endif
#endif
            _c4exp_free(e);
        }

        /// <summary>
        /// Closes the expiration enumerator
        /// </summary>
        /// <param name="e">The enumerator to close</param>
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public static extern void c4exp_close(C4ExpiryEnumerator* e);

        /// <summary>
        /// Advances the enumerator to the next document.
        /// Returns false at the end, or on error; look at the C4Error to determine which occurred,
        /// and don't forget to free the enumerator.
        /// </summary>
        /// <returns>true if advanced, or false otherwise</returns>
        /// <param name="e">The enumerator to operate on</param>
        /// <param name="outError">The error, if any</param>
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4enum_next(C4DocEnumerator* e, C4Error* outError);

        /// <summary>
        /// Gets metadata about the document from the current doc enumerator
        /// </summary>
        /// <returns>Whether or not the information was successfully retrieved</returns>
        /// <param name="e">The enumerator to query</param>
        /// <param name="info">The document metadata that was retrieved</param>
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4enum_getDocumentInfo(C4DocEnumerator* e, C4DocumentInfo* info);

        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi, EntryPoint = "c4enum_getDocument")]
        private static extern C4Document* _c4enum_getDocument(C4DocEnumerator* e, C4Error* outError);

        /// <summary>
        /// Returns the current document, if any, from an enumerator.
        /// </summary>
        /// <returns>The document, or NULL if there is none or if an error occurred reading its body.
        /// Caller is responsible for calling c4document_free when done with it</returns>
        /// <param name="e">The enumerator</param>
        /// <param name="outError">Error will be stored here on failure</param>
        public static C4Document* c4enum_getDocument(C4DocEnumerator* e, C4Error* outError)
        {
#if DEBUG && !NET_3_5
            var retVal = _c4enum_getDocument(e, outError);
            if(retVal != null) {
                _AllocatedObjects.TryAdd((IntPtr)retVal, "C4Document");
            #if TRACE
            Trace.WriteLine("[c4enum_getDocument] Allocated 0x{0}", ((IntPtr)retVal).ToString("X"));
#endif
            }

            return retVal;
#else
            return _c4enum_getDocument(e, outError);
#endif
        }

        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi, EntryPoint = "c4enum_nextDocument")]
        private static extern C4Document* _c4enum_nextDocument(C4DocEnumerator* e, C4Error* outError);

        /// <summary>
        /// Convenience function that combines c4enum_next() and c4enum_getDocument()
        /// </summary>
        /// <param name="e">The enumerator to operate on</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>A pointer to the document on success, otherwise null</returns>
        public static C4Document* c4enum_nextDocument(C4DocEnumerator* e, C4Error* outError)
        {
#if DEBUG && !NET_3_5
            var retVal = _c4enum_nextDocument(e, outError);
            if(retVal != null) {
                _AllocatedObjects.TryAdd((IntPtr)retVal, "C4Document");
                #if TRACE
                Trace.WriteLine("[c4enum_nextDocument] Allocated 0x{0}", ((IntPtr)retVal).ToString("X"));
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
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public static extern int c4doc_insertRevision(C4Document* doc, C4Slice revID, C4Slice body,
            [MarshalAs(UnmanagedType.U1)]bool deleted, [MarshalAs(UnmanagedType.U1)]bool hasAttachments,
            [MarshalAs(UnmanagedType.U1)]bool allowConflict, C4Error* outError);

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
        public static int c4doc_insertRevision(C4Document* doc, string revID, string body, bool deleted, bool hasAttachments,
            bool allowConflict, C4Error* outError)
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
        /// </summary>
        /// <param name="doc">The document to operate on</param>
        /// <param name="body">The (JSON) body of the revision</param>
        /// <param name="deleted">True if this revision is a deletion (tombstone)</param>
        /// <param name="hasAttachments">True if this revision contains an _attachments dictionary</param>
        /// <param name="history">The ancestors' revision IDs, starting with the parent, in reverse order</param>
        /// <param name="historyCount">The number of objects in the history array</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>The number of revisions added to the document, or -1 on error.</returns>
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public static extern int c4doc_insertRevisionWithHistory(C4Document* doc, C4Slice body,
            [MarshalAs(UnmanagedType.U1)]bool deleted, [MarshalAs(UnmanagedType.U1)]bool hasAttachments, C4Slice* history,
            uint historyCount, C4Error* outError);

        /// <summary>
        ///  Adds a revision to a document, plus its ancestors (given in reverse chronological order.)
        /// On success, the new revision will be selected.
        /// Must be called within a transaction.
        /// </summary>
        /// <param name="doc">The document to operate on</param>
        /// <param name="body">The (JSON) body of the revision</param>
        /// <param name="deleted">True if this revision is a deletion (tombstone)</param>
        /// <param name="hasAttachments">True if this revision contains an _attachments dictionary</param>
        /// <param name="history">The ancestors' revision IDs, starting with the parent, in reverse order</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>The number of revisions added to the document, or -1 on error.</returns>
        public static int c4doc_insertRevisionWithHistory(C4Document* doc, string body, bool deleted,
            bool hasAttachments, string[] history, C4Error* outError)
        {
            var flattenedStringArray = new C4String[history.Length + 1];
            flattenedStringArray[0] = new C4String(body);
            for(int i = 0; i < history.Length; i++) {
                flattenedStringArray[i + 1] = new C4String(history[i]);
            }

            var sliceArray = flattenedStringArray.Skip(1).Select<C4String, C4Slice>(x => x.AsC4Slice()).ToArray();
            var retVal = default(int);
            fixed (C4Slice* a = sliceArray)
            {
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
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public static extern void c4doc_setType(C4Document* doc, C4Slice docType);

        /// <summary>
        /// Sets a document's docType. (By convention this is the value of the "type" property of the 
        /// current revision's JSON; this value can be used as optimization when indexing a view.)
        /// The change will not be persisted until the document is saved.
        /// </summary>
        /// <param name="doc">The document to operate on</param>
        /// <param name="docType">The document type to set</param>
        /// <returns>true on success, false otherwise</returns>
        public static void c4doc_setType(C4Document* doc, string docType)
        {
            using(var docType_ = new C4String(docType)) {
                c4doc_setType(doc, docType_.AsC4Slice());
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
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public static extern int c4doc_purgeRevision(C4Document* doc, C4Slice revId, C4Error* outError);

        /// <summary>
        /// Purges a revision from a document.  As with purgeDoc, this operation
        /// is immediate and non-replicating.
        /// </summary>
        /// <returns>1 if the revision was purged, 0 if the revision didn't exist,
        /// or -1 on error</returns>
        /// <param name="doc">The document to modify.</param>
        /// <param name="revId">The ID of the revision to purge</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        public static int c4doc_purgeRevision(C4Document* doc, string revId, C4Error* outError)
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
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4doc_save(C4Document* doc, uint maxRevTreeDepth, C4Error* outError);

        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        internal static extern C4Document* c4doc_put(C4Database* database, C4DocPutRequest_Internal* request,
            UIntPtr* outCommonAncestorIndex, C4Error* outError);

        internal static C4Document* c4doc_put(C4Database* database, C4DocPutRequest request,
            UIntPtr* outCommonAncestorIndex, C4Error* outError)
        {
            C4Document* retVal = null;
            request.AsInternalObject(internalObject =>
            {
                retVal = c4doc_put(database, &internalObject, outCommonAncestorIndex, outError);
#if DEBUG && !NET_3_5
                if(retVal != null) {
                    _AllocatedObjects.TryAdd((IntPtr)retVal, "C4Document");
                    #if TRACE
                    Trace.WriteLine("[c4enum_nextDocument] Allocated 0x{0}", ((IntPtr)retVal).ToString("X"));
#endif
                }
#endif
            });

            return retVal;
        }

        /// <summary>
        /// A high-level Put operation, to insert a new or downloaded revision.
        /// * If request->existingRevision is true, then request->history must contain the revision's
        ///   history, with the revision's ID as the first item.
        /// * Otherwise, a new revision will be created and assigned a revID.The parent revision ID,
        ///   if any, should be given as the single item of request->history.
        /// Either way, on success the document is returned with the inserted revision selected.
        /// Note that actually saving the document back to the database is optional -- it only happens
        /// if request->save is true. You can set this to false if you want to review the changes
        /// before saving, e.g.to run them through a validation function.
        /// </summary>
        /// <returns>The created document</returns>
        /// <param name="database">The database to insert into</param>
        /// <param name="request">The information about the document to insert</param>
        /// <param name="outCommonAncestorIndex">The index of the existing ancestor of the document in the database</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        public static C4Document* c4doc_put(C4Database* database, C4DocPutRequest request,
            ulong* outCommonAncestorIndex, C4Error* outError)
        {
            UIntPtr outValue;
            var retVal = c4doc_put(database, request, &outValue, outError);
            if(outCommonAncestorIndex != null) {
                *outCommonAncestorIndex = outValue.ToUInt64();
            }

            return retVal;
        }

        /// <summary>
        /// Sets an expiration date on a document.  After this time the
        /// </summary>
        /// <returns><c>true</c>, on success, <c>false</c> on failure.</returns>
        /// <param name="db">The database to set the expiration date in</param>
        /// <param name="docID">The ID of the document to set the expiration date for.</param>
        /// <param name="timestamp">timestamp The UNIX timestamp of the expiration date (must
        /// be in the future, i.e. after the current value of DateTime.UtcNow).</param>
        /// <param name="outError">Information about any error that occurred</param>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4doc_setExpiration(C4Database *db, C4Slice docID, ulong timestamp, C4Error *outError);

        /// <summary>
        /// Sets an expiration date on a document.  After this time the
        /// </summary>
        /// <returns><c>true</c>, on success, <c>false</c> on failure.</returns>
        /// <param name="db">The database to set the expiration date in</param>
        /// <param name="docID">The ID of the document to set the expiration date for.</param>
        /// <param name="timestamp">timestamp The UNIX timestamp of the expiration date (must
        /// be in the future, i.e. after the current value of DateTime.UtcNow).</param>
        /// <param name="outError">Information about any error that occurred</param>
        public static bool c4doc_setExpiration(C4Database *db, string docID, ulong timestamp, C4Error *outError)
        {
            using (var docID_ = new C4String(docID)) {
                return c4doc_setExpiration(db, docID_.AsC4Slice(), timestamp, outError);
            }
        }

        /// <summary>
        /// Gets the expiration date of the document with the given ID
        /// </summary>
        /// <returns>The expiration date as a unix timestamp</returns>
        /// <param name="db">The database to check</param>
        /// <param name="docID">The ID of the document to check</param>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        public static extern ulong c4doc_getExpiration(C4Database *db, C4Slice docID);

        /// <summary>
        /// Gets the expiration date of the document with the given ID
        /// </summary>
        /// <returns>The expiration date as a unix timestamp</returns>
        /// <param name="db">The database to check</param>
        /// <param name="docID">The ID of the document to check</param>
        public static ulong c4doc_getExpiration(C4Database* db, string docID)
        {
            using (var docID_ = new C4String(docID)) {
                return c4doc_getExpiration(db, docID_.AsC4Slice());
            }
        }

        /// <summary>
        /// Gets the time of the next document scheduled for expiration (if any)
        /// </summary>
        /// <returns>The time of the next document scheduled for expiration as a unix timestamp</returns>
        /// <param name="db">The database to query</param>
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public static extern ulong c4db_nextDocExpiration(C4Database* db);

        /// <summary>
        /// Generates the revision ID for a new document revision.
        /// </summary>
        /// <returns>The new revID. Caller is responsible for freeing its buf.</returns>
        /// <param name="body">The (JSON) body of the revision, exactly as it'll be stored.</param>
        /// <param name="parentRevID">The revID of the parent revision, or null if there's none.</param>
        /// <param name="deletion"><c>true</c> if this revision is a deletion.</param>
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi, EntryPoint = "c4doc_generateRevID")]
        public static extern C4Slice _c4doc_generateRevID(C4Slice body, C4Slice parentRevID,
            [MarshalAs(UnmanagedType.U1)]bool deletion);

        /// <summary>
        /// Generates the revision ID for a new document revision.
        /// </summary>
        /// <returns>The new revID.</returns>
        /// <param name="body">The (JSON) body of the revision, exactly as it'll be stored.</param>
        /// <param name="parentRevID">The revID of the parent revision, or null if there's none.</param>
        /// <param name="deletion"><c>true</c> if this revision is a deletion.</param>
        public static string c4doc_generateRevID(C4Slice body, C4Slice parentRevID, bool deletion)
        {
            return BridgeSlice(() => _c4doc_generateRevID(body, parentRevID, deletion));
        }

        /// <summary>
        /// Generates the revision ID for a new document revision.
        /// </summary>
        /// <returns>The new revID.</returns>
        /// <param name="body">The (JSON) body of the revision, exactly as it'll be stored.</param>
        /// <param name="parentRevID">The revID of the parent revision, or null if there's none.</param>
        /// <param name="deletion"><c>true</c> if this revision is a deletion.</param>
        public static string c4doc_generateRevID(string body, string parentRevID, bool deletion)
        {
            using(var body_ = new C4String(body))
            using(var parentRevID_ = new C4String(parentRevID)) {
                return c4doc_generateRevID(body_.AsC4Slice(), parentRevID_.AsC4Slice(), deletion);
            }
        }

        /// <summary>
        /// Set whether or not to generate the rev IDs for Couchbase Lite 1.0 - 1.2 (using MD5) instead of
        /// the current way (using SHA)
        /// </summary>
        /// <param name="generateOldStyle"><c>false</c> (default) to generate new style, and <c>true</c>
        ///  to generate old style</param>
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public static extern void c4doc_generateOldStyleRevID([MarshalAs(UnmanagedType.U1)]bool generateOldStyle);

        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi, EntryPoint = "c4key_new")]
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
                #if TRACE
                Trace.WriteLine("[c4key_new] Allocated 0x{0}", ((IntPtr)retVal).ToString("X"));
#endif
            }

            return retVal;
#else
            return _c4key_new();
#endif
        }

        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi, EntryPoint = "c4key_withBytes")]
        private static extern C4Key* _c4key_withBytes(C4Slice slice);

        /// <summary>
        /// Creates a C4Key by copying the data, which must be in the C4Key binary format.
        /// </summary>
        /// <param name="slice">The data to use in the C4Key</param>
        /// <returns>A pointer to the created C4Key</returns>
        public static C4Key* c4key_withBytes(C4Slice slice)
        {
#if DEBUG && !NET_3_5
            var retVal = _c4key_withBytes(slice);
            if(retVal != null) {
                _AllocatedObjects.TryAdd((IntPtr)retVal, "C4Key");
                #if TRACE
                Trace.WriteLine("[c4key_withBytes] Allocated 0x{0}", ((IntPtr)retVal).ToString("X"));
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
            fixed (byte* ptr = realized)
            {
                var slice = new C4Slice(ptr, (uint)realized.Length);
                return c4key_withBytes(slice);
            }
        }

        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi, EntryPoint = "c4key_newFullTextString")]
        private static extern C4Key* _c4key_newFullTextString(C4Slice text, C4Slice language);

        /// <summary>
        /// Creates a C4Key containing a string of text to be full-text-indexed by a view.
        /// </summary>
        /// <returns>A new C4Key representing this key.</returns>
        /// <param name="language">The human language of the string as an ISO-639 code like "en";
        /// or kC4LanguageNone to disable language-specific transformations such as
        /// stemming; or kC4LanguageDefault to fall back to the default language
        /// (as set by c4key_setDefaultFullTextLanguage.)</param>
        /// <param name="text">The text to be indexed.</param>
        public static C4Key* c4key_newFullTextString(C4Slice text, C4Slice language)
        {
#if DEBUG && !NET_3_5
            var retVal = _c4key_newFullTextString(text, language);
            if(retVal != null) {
                _AllocatedObjects.TryAdd((IntPtr)retVal, "C4Key");
                #if TRACE
                Trace.WriteLine("[c4key_withBytes] Allocated 0x{0}", ((IntPtr)retVal).ToString("X"));
#endif
            }

            return retVal;
#else
            return _c4key_newFullTextString(text, language);
#endif
        }

        /// <summary>
        /// Creates a C4Key containing a string of text to be full-text-indexed by a view.
        /// </summary>
        /// <returns>A new C4Key representing this key.</returns>
        /// <param name="language">The human language of the string as an ISO-639 code like "en";
        /// or C4Language.None to disable language-specific transformations such as
        /// stemming; or C4Languaage.Default to fall back to the default language
        /// (as set by c4key_setDefaultFullTextLanguage.)</param>
        /// <param name="text">The text to be indexed.</param>
        public static C4Key* c4key_newFullTextString(string text, string language)
        {
            using(var text_ = new C4String(text))
            using(var language_ = new C4String(language)) {
                return c4key_newFullTextString(text_.AsC4Slice(), language_.AsC4Slice());
            }
        }

        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi, EntryPoint = "c4key_newGeoJSON")]
        private static extern C4Key* _c4key_newGeoJSON(C4Slice geoJSON, C4GeoArea boundingBox);

        /// <summary>
        /// Creates a C4Key containing a 2D shape to be geo-indexed.
        /// Caller must provide the bounding box of the shape.
        /// </summary>
        /// <returns>A new C4Key for the shape.</returns>
        /// <param name="geoJSON">GeoJSON describing the shape.</param>
        /// <param name="boundingBox">A conservative bounding box of the shape.</param>
        public static C4Key* c4key_newGeoJSON(C4Slice geoJSON, C4GeoArea boundingBox)
        {
#if DEBUG && !NET_3_5
            var retVal = _c4key_newGeoJSON(geoJSON, boundingBox);
            if(retVal != null) {
                _AllocatedObjects.TryAdd((IntPtr)retVal, "C4Key");
                #if TRACE
                Trace.WriteLine("[c4key_withBytes] Allocated 0x{0}", ((IntPtr)retVal).ToString("X"));
#endif
            }

            return retVal;
#else
            return _c4key_newGeoJSON(geoJSON, boundingBox);
#endif
        }

        /// <summary>
        /// Creates a C4Key containing a 2D shape to be geo-indexed.
        /// Caller must provide the bounding box of the shape.
        /// </summary>
        /// <returns>A new C4Key for the shape.</returns>
        /// <param name="geoJSON">GeoJSON describing the shape.</param>
        /// <param name="boundingBox">A conservative bounding box of the shape.</param>
        public static C4Key* c4key_newGeoJSON(string geoJSON, C4GeoArea boundingBox)
        {
            using(var geoJSON_ = new C4String(geoJSON)) {
                return c4key_newGeoJSON(geoJSON_.AsC4Slice(), boundingBox);
            }
        }

        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi, EntryPoint = "c4key_free")]
        private static extern void _c4key_free(C4Key* key);

        /// <summary>
        /// Frees a C4Key.
        /// </summary>
        /// <param name="key">The key to free</param>
        public static void c4key_free(C4Key* key)
        {
#if DEBUG && !NET_3_5
            var ptr = (IntPtr)key;
            #if TRACE
            if(ptr != IntPtr.Zero && !_AllocatedObjects.ContainsKey(ptr)) {
                Trace.WriteLine("WARNING: [c4key_free] freeing object 0x{0} that was not found in allocated list", ptr.ToString("X"));
            } else {
#endif
                _AllocatedObjects.TryRemove(ptr, out _Dummy);
            #if TRACE
            }
#endif
#endif
            _c4key_free(key);
        }

        /// <summary>
        /// Adds a JSON null value to a C4Key.
        /// </summary>
        /// <param name="key">The key to operate on</param>
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public static extern void c4key_addNull(C4Key* key);

        /// <summary>
        /// Adds a boolean value to a C4Key.
        /// </summary>
        /// <param name="key">he key to operate on</param>
        /// <param name="b">The value to store</param>
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public static extern void c4key_addBool(C4Key* key, [MarshalAs(UnmanagedType.U1)]bool b);

        /// <summary>
        /// Adds a number to a C4Key.
        /// </summary>
        /// <param name="key">The key to operate on</param>
        /// <param name="d">The value to store</param>
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public static extern void c4key_addNumber(C4Key* key, double d);

        /// <summary>
        /// Adds a UTF-8 string to a C4Key.
        /// </summary>
        /// <param name="key">The key to operate on></param>
        /// <param name="s">The value to store</param>
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public static extern void c4key_addString(C4Key* key, C4Slice s);

        /// <summary>
        /// Adds a UTF-8 string to a C4Key.
        /// </summary>
        /// <param name="key">The key to operate on></param>
        /// <param name="s">The value to store</param>
        public static void c4key_addString(C4Key* key, string s)
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
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public static extern void c4key_addMapKey(C4Key* key, C4Slice s);

        /// <summary>
        /// Adds a map key, before the next value. When adding to a map, every value must be
        /// preceded by a key.
        /// </summary>
        /// <param name="key">The key to operate on</param>
        /// <param name="s">The value to store</param>
        public static void c4key_addMapKey(C4Key* key, string s)
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
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public static extern void c4key_beginArray(C4Key* key);

        /// <summary>
        /// Closes an array opened by c4key_beginArray. (Every array must be closed.)
        /// </summary>
        /// <param name="key">The key to operate on</param>
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public static extern void c4key_endArray(C4Key* key);

        /// <summary>
        /// Adds a map/dictionary/object to a C4Key.
        /// Subsequent keys and values added will go into the map, until c4key_endMap is called.
        /// </summary>
        /// <param name="key">The key to operate on</param>
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public static extern void c4key_beginMap(C4Key* key);

        /// <summary>
        /// Closes a map opened by c4key_beginMap. (Every map must be closed.)
        /// </summary>
        /// <param name="key">The key to operate on</param>
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public static extern void c4key_endMap(C4Key* key);


        /// <summary>
        /// Sets the process-wide default (human) language for full-text keys. This affects how
        /// words are "stemmed" (stripped of suffixes like "-ing" or "-est" in English) when indexed.
        /// </summary>
        /// <returns><c>true</c>, if the languageName was recognized, <c>false</c> if not.</returns>
        /// <param name="languageName">An ISO language name like 'english'</param>
        /// <param name="stripDiacriticals"><c>true</c> if accents and other diacriticals should be stripped from
        /// letters. Appropriate for English but not for most other languages.</param>
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4key_setDefaultFullTextLanguage(C4Slice languageName,
            [MarshalAs(UnmanagedType.U1)]bool stripDiacriticals);

        /// <summary>
        /// Sets the process-wide default (human) language for full-text keys. This affects how
        /// words are "stemmed" (stripped of suffixes like "-ing" or "-est" in English) when indexed.
        /// </summary>
        /// <returns><c>true</c>, if the languageName was recognized, <c>false</c> if not.</returns>
        /// <param name="languageName">An ISO language name like 'english'</param>
        /// <param name="stripDiacriticals"><c>true</c> if accents and other diacriticals should be stripped from
        /// letters. Appropriate for English but not for most other languages.</param>
        public static bool c4key_setDefaultFullTextLanguage(string languageName, bool stripDiacriticals)
        {
            using(var languageName_ = new C4String(languageName)) {
                return c4key_setDefaultFullTextLanguage(languageName_.AsC4Slice(), stripDiacriticals);
            }
        }

        /// <summary>
        /// Returns a C4KeyReader that can parse the contents of a C4Key.
        /// Warning: Adding to the C4Key will invalidate the reader.
        /// </summary>
        /// <param name="key">The key to operate on</param>
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public static extern C4KeyReader c4key_read(C4Key* key);

        /// <summary>
        /// Returns the type of the next item in the key, or kC4Error at the end of the key or if the
        /// data is corrupt.
        /// To move on to the next item, you must call skipToken or one of the read___ functions.
        /// </summary>
        /// <param name="reader">The reader to operate on</param>
        /// <returns>The type of the next item in the key</returns>
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public static extern C4KeyToken c4key_peek(C4KeyReader* reader);

        /// <summary>
        /// Skips the current token in the key. If it was kC4Array or kC4Map, the reader will
        /// now be positioned at the first item of the collection.
        /// </summary>
        /// <param name="reader">The reader to operate on</param>
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public static extern void c4key_skipToken(C4KeyReader* reader);

        /// <summary>
        /// Reads a boolean value.
        /// </summary>
        /// <param name="reader">The reader to operate on</param>
        /// <returns>The boolean value of the next token of the key</returns>
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4key_readBool(C4KeyReader* reader);

        /// <summary>
        /// Reads a number value
        /// </summary>
        /// <param name="reader">The reader to operate on</param>
        /// <returns>The numerical value of the next token of the key</returns>
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public static extern double c4key_readNumber(C4KeyReader* reader);

        /// <summary>
        /// Reads a string value
        /// </summary>
        /// <param name="reader">The reader to operate on</param>
        /// <returns>The string value of the next token of the key</returns>
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi, EntryPoint = "c4key_readString")]
        public static extern C4Slice _c4key_readString(C4KeyReader* reader);

        /// <summary>
        /// Reads a string value
        /// </summary>
        /// <param name="reader">The reader to operate on</param>
        /// <returns>The string value of the next token of the key</returns>
        public static string c4key_readString(C4KeyReader* reader)
        {
            return BridgeSlice(() => _c4key_readString(reader));
        }

        /// <summary>
        /// Converts a C4KeyReader to JSON.
        /// </summary>
        /// <param name="reader">The reader to operate on</param>
        /// <returns>The JSON string result</returns>
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi, EntryPoint = "c4key_toJSON")]
        public static extern C4Slice _c4key_toJSON(C4KeyReader* reader);

        /// <summary>
        /// Converts a C4KeyReader to JSON.
        /// </summary>
        /// <param name="reader">The reader to operate on</param>
        /// <returns>The JSON string result</returns>
        public static string c4key_toJSON(C4KeyReader* reader)
        {
            return BridgeSlice(() => _c4key_toJSON(reader));
        }

        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi, EntryPoint = "c4view_open")]
        private static extern C4View* _c4view_open(C4Database* db, C4Slice path, C4Slice viewName, C4Slice version,
            C4DatabaseFlags flags, C4EncryptionKey* encryptionKey, C4Error* outError);

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
        public static C4View* c4view_open(C4Database* db, C4Slice path, C4Slice viewName, C4Slice version,
            C4DatabaseFlags flags, C4EncryptionKey* encryptionKey, C4Error* outError)
        {
#if DEBUG && !NET_3_5
            var retVal = _c4view_open(db, path, viewName, version, flags, encryptionKey, outError);
            if(retVal != null) {
                _AllocatedObjects.TryAdd((IntPtr)retVal, "C4View");
                #if TRACE
                Trace.WriteLine("[c4view_open] Allocated 0x{0}", ((IntPtr)retVal).ToString("X"));
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
        public static C4View* c4view_open(C4Database* db, string path, string viewName, string version, C4DatabaseFlags flags,
            C4EncryptionKey* encryptionKey, C4Error* outError)
        {
            using(var path_ = new C4String(path))
            using(var viewName_ = new C4String(viewName))
            using(var version_ = new C4String(version)) {
                return c4view_open(db, path_.AsC4Slice(), viewName_.AsC4Slice(), version_.AsC4Slice(), flags, encryptionKey, outError);
            }
        }

        /// <summary>
        /// Closes a given view.  Any further access to it is invalid.
        /// </summary>
        /// <returns>Whether or not the close succeeded</returns>
        /// <param name="view">The view to close.</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4view_close(C4View* view, C4Error* outError);

        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi, EntryPoint = "c4view_free")]
        private static extern void _c4view_free(C4View* view);

        /// <summary>
        /// Frees the given view.
        /// </summary>
        /// <param name="view">The view to free</param>
        /// <returns>true on success, false otherwise</returns>
        public static void c4view_free(C4View* view)
        {
#if DEBUG && !NET_3_5
            var ptr = (IntPtr)view;
            #if TRACE
            if(ptr != IntPtr.Zero && !_AllocatedObjects.ContainsKey(ptr)) {
                Trace.WriteLine("WARNING: [c4view_close] freeing object 0x{0} that was not found in allocated list", ptr.ToString("X"));
            } else {
#endif
                _AllocatedObjects.TryRemove(ptr, out _Dummy);
            #if TRACE
            }
#endif
#endif
            _c4view_free(view);
        }

        /// <summary>
        /// Erases the view index, but doesn't delete the database file.
        /// </summary>
        /// <param name="view">The view that will have its index erased</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>true on success, false otherwise</returns>
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4view_eraseIndex(C4View* view, C4Error* outError);

        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi, EntryPoint = "c4view_delete")]
        [return: MarshalAs(UnmanagedType.U1)]
        private static extern bool _c4view_delete(C4View* view, C4Error* outError);

        /// <summary>
        /// Deletes the view file and closes/frees the C4View.
        /// </summary>
        /// <param name="view">The view to operate on</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>true on success, false otherwise</returns>
        public static bool c4view_delete(C4View* view, C4Error* outError)
        {
#if DEBUG && !NET_3_5
            var ptr = (IntPtr)view;
            #if TRACE
            if(ptr != IntPtr.Zero && !_AllocatedObjects.ContainsKey(ptr)) {
                Trace.WriteLine("WARNING: [c4view_delete] freeing object 0x{0} that was not found in allocated list", ptr.ToString("X"));
            } else {
#endif
                _AllocatedObjects.TryRemove(ptr, out _Dummy);
            #if TRACE
            }
#endif
#endif
            return _c4view_delete(view, outError);
        }

        /// <summary>
        /// Deletes the file(s) for the view at the given path.
        /// All C4Views at that path should be closed first.
        /// </summary>
        /// <returns><c>true</c>, if delete at path was c4viewed, <c>false</c> otherwise.</returns>
        /// <param name="path">The path to delete files at</param>
        /// <param name="flags">The flags to use during deletion</param>
        /// <param name="outError">Any errors that occurred will be recorded here</param> 
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4view_deleteAtPath(C4Slice path, C4DatabaseFlags flags, C4Error* outError);

        /// <summary>
        /// Deletes the file(s) for the view at the given path.
        /// All C4Databases at that path should be closed first.
        /// </summary>
        /// <returns>Whether or not the operation succeeded</returns>
        /// <param name="path">The path to delete at.</param>
        /// <param name="flags">The flags for closing.</param>
        /// <param name="outError">Any errors that occurred will be recorded here</param> 
        public static bool c4view_deleteAtPath(string path, C4DatabaseFlags flags, C4Error* outError)
        {
            using(var path_ = new C4String(path)) {
                return c4view_deleteAtPath(path_.AsC4Slice(), flags, outError);
            }
        }

        /// <summary>
        /// Sets the version of the map function on the given view.  Any change to the version
        /// will result in the index being invalidated.
        /// </summary>
        /// <param name="view">The view to set the map version on.</param>
        /// <param name="version">The version to set.</param>
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public static extern void c4view_setMapVersion(C4View* view, C4Slice version);

        /// <summary>
        /// Sets the version of the map function on the given view.  Any change to the version
        /// will result in the index being invalidated.
        /// </summary>
        /// <param name="view">The view to set the map version on.</param>
        /// <param name="version">The version to set.</param>
        public static void c4view_setMapVersion(C4View* view, string version)
        {
            using(var version_ = new C4String(version)) {
                c4view_setMapVersion(view, version_.AsC4Slice());
            }
        }

        /// <summary>
        /// Returns the total number of rows in the view index.
        /// </summary>
        /// <param name="view">The view to operate on</param>
        /// <returns>The total number of rows in the view index</returns>
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public static extern ulong c4view_getTotalRows(C4View* view);

        /// <summary>
        /// Returns the last database sequence number that's been indexed.
        /// If this is less than the database's lastSequence, the view index is out of date.
        /// </summary>
        /// <param name="view">The view to operate on</param>
        /// <returns>The last database sequence number that's been indexed</returns>
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public static extern ulong c4view_getLastSequenceIndexed(C4View* view);

        /// <summary>
        /// Returns the last database sequence number that changed the view index.
        /// </summary>
        /// <param name="view">The view to operate on</param>
        /// <returns>The last database sequence number that changed the view index</returns>
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public static extern ulong c4view_getLastSequenceChangedAt(C4View* view);

        /// <summary>
        /// Sets the document type for a given view
        /// </summary>
        /// <param name="view">The view to operate on.</param>
        /// <param name="docType">The document type to set</param>
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public static extern void c4view_setDocumentType(C4View* view, C4Slice docType);

        /// <summary>
        /// Sets the document type for a given view
        /// </summary>
        /// <param name="view">The view to operate on.</param>
        /// <param name="docType">The document type to set</param>
        public static void c4view_setDocumentType(C4View* view, string docType)
        {
            using(var docType_ = new C4String(docType)) {
                c4view_setDocumentType(view, docType_.AsC4Slice());
            }
        }

        /// <summary>
        /// Changes the encryption key on a given view
        /// </summary>
        /// <returns><c>true</c>, if the operation succeeded, <c>false</c> otherwise.</returns>
        /// <param name="view">The view to change the encryption key of.</param>
        /// <param name="newKey">The new encryption key to use</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4view_rekey(C4View* view, C4EncryptionKey* newKey, C4Error* outError);

        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi, EntryPoint = "c4indexer_begin")]
        private static extern C4Indexer* _c4indexer_begin(C4Database* db, C4View** views, UIntPtr viewCount, C4Error* outError);

        /// <summary>
        /// Creates an indexing task on one or more views in a database.
        /// </summary>
        /// <param name="db">The database to operate on</param>
        /// <param name="views">An array of views whose indexes should be updated in parallel.</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>A pointer to the indexer on success, otherwise null</returns>
        public static C4Indexer* c4indexer_begin(C4Database* db, C4View*[] views, C4Error* outError)
        {
            fixed (C4View** viewPtr = views)
            {
#if DEBUG && !NET_3_5
                var retVal = _c4indexer_begin(db, viewPtr, (UIntPtr)(uint)views.Length, outError);
                if(retVal != null) {
                    _AllocatedObjects.TryAdd((IntPtr)retVal, "C4Indexer");
                #if TRACE
                    Trace.WriteLine("[c4indexer_begin] Allocated 0x{0}", ((IntPtr)retVal).ToString("X"));
#endif
                }

                return retVal;
#else
                return _c4indexer_begin(db, viewPtr, (UIntPtr)(uint)views.Length, outError);
#endif
            }
        }

        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi, EntryPoint = "c4indexer_enumerateDocuments")]
        private static extern C4DocEnumerator* _c4indexer_enumerateDocuments(C4Indexer* indexer, C4Error* outError);

        /// <summary>
        /// Enumerate the documents that still need to be indexed in a given indexer
        /// </summary>
        /// <returns>The enumerator, or null on failure</returns>
        /// <param name="indexer">The indexer to check for documents.</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        public static C4DocEnumerator* c4indexer_enumerateDocuments(C4Indexer* indexer, C4Error* outError)
        {
#if DEBUG && !NET_3_5
            var retVal = _c4indexer_enumerateDocuments(indexer, outError);
            if(retVal != null) {
                _AllocatedObjects.TryAdd((IntPtr)retVal, "C4DocEnumerator");
                #if TRACE
                Trace.WriteLine("[c4indexer_enumerateDocuments] Allocated 0x{0}", ((IntPtr)retVal).ToString("X"));
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
        /// <param name="document">The document to use during the emit</param>
        /// <param name="viewNumber">The index of the view in the views passed to the indexer</param>
        /// <param name="emitCount">The number of elements on the emittedKeys and emittedValues</param>
        /// <param name="emittedKeys">The keys emitted by the map function</param>
        /// <param name="emittedValues">The values emitted by the map function</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>A pointer to the enumerator on success, otherwise null</returns>
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4indexer_emit(C4Indexer* indexer, C4Document* document, uint viewNumber, uint emitCount,
            C4Key** emittedKeys, C4Slice* emittedValues, C4Error* outError);

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
        public static bool c4indexer_emit(C4Indexer* indexer, C4Document* document, uint viewNumber,
            C4Key*[] emittedKeys, C4Slice[] emittedValues, C4Error* outError)
        {
            fixed (C4Key** keysPtr = emittedKeys)
            fixed (C4Slice* valuesPtr = emittedValues)
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
        public static bool c4indexer_emit(C4Indexer* indexer, C4Document* document, uint viewNumber,
            C4Key*[] emittedKeys, string[] emittedValues, C4Error* outError)
        {
            var c4StringArr = emittedValues.Select(x => new C4String(x)).ToArray();
            var sliceArr = c4StringArr.Select(x => x.AsC4Slice()).ToArray();
            var retVal = c4indexer_emit(indexer, document, viewNumber, emittedKeys, sliceArr, outError);
            foreach(var c4str in c4StringArr) {
                c4str.Dispose();
            }

            return retVal;
        }

        /// <summary>
        /// Emits a preconfigured key value list to the given indexer
        /// </summary>
        /// <returns>Whether or not the operation succeeded</returns>
        /// <param name="indexer">The indexer to operate on</param>
        /// <param name="document">The document being indexed</param>
        /// <param name="viewNumber">The position of the view in the indexer's views[] array</param>
        /// <param name="kv">The list of emitted keys and values</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4indexer_emitList(C4Indexer* indexer, C4Document* document, uint viewNumber,
            C4KeyValueList* kv, C4Error* outError);

        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi, EntryPoint = "c4indexer_end")]
        [return: MarshalAs(UnmanagedType.U1)]
        private static extern bool _c4indexer_end(C4Indexer* indexer, [MarshalAs(UnmanagedType.U1)]bool commit, C4Error* outError);

        /// <summary>
        /// Finishes an indexing task and frees the indexer reference.
        /// </summary>
        /// <param name="indexer">The indexer to operate on</param>
        /// <param name="commit">True to commit changes to the indexes, false to abort</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>true on success, false otherwise</returns>
        public static bool c4indexer_end(C4Indexer* indexer, bool commit, C4Error* outError)
        {
#if DEBUG && !NET_3_5
            var ptr = (IntPtr)indexer;
            #if TRACE
            if(ptr != IntPtr.Zero && !_AllocatedObjects.ContainsKey(ptr)) {
                Trace.WriteLine("WARNING: [c4indexer_end] freeing object 0x{0} that was not found in allocated list", ptr.ToString("X"));
            } else {
#endif
                _AllocatedObjects.TryRemove(ptr, out _Dummy);
            #if TRACE
            }
#endif
#endif
            return _c4indexer_end(indexer, commit, outError);
        }

        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi, EntryPoint = "c4view_query")]
        private static extern C4QueryEnumerator* _c4view_query(C4View* view, C4QueryOptions* options, C4Error* outError);

        /// <summary>
        /// Runs a query and returns an enumerator for the results.
        /// The enumerator's fields are not valid until you call c4queryenum_next(), though.
        /// </summary>
        /// <param name="view">The view to operate on</param>
        /// <param name="options">The query options</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>A pointer to the enumerator on success, otherwise null</returns>
        public static C4QueryEnumerator* c4view_query(C4View* view, C4QueryOptions* options, C4Error* outError)
        {
#if DEBUG && !NET_3_5
            var retVal = _c4view_query(view, options, outError);
            if(retVal != null) {
                _AllocatedObjects.TryAdd((IntPtr)retVal, "C4QueryEnumerator");
                #if TRACE
                Trace.WriteLine("[c4view_query] Allocated 0x{0}", ((IntPtr)retVal).ToString("X"));
#endif
            }

            return retVal;
#else
            return _c4view_query(view, options, outError);
#endif
        }

        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi, EntryPoint = "c4view_fullTextQuery")]
        private static extern C4QueryEnumerator* _c4view_fullTextQuery(C4View* view, C4Slice queryString,
            C4Slice queryStringLanguage, C4QueryOptions* options, C4Error* outError);

        /// <summary>
        /// Runs a full-text query and returns an enumerator for the results.
        /// </summary>
        /// <returns>A new query enumerator. Fields are invalid until c4queryenum_next is called.</returns>
        /// <param name="view">The view to query.</param>
        /// <param name="queryString">A string containing the words to search for, separated by whitespace.</param>
        /// <param name="queryStringLanguage">The human language of the query string as an ISO-639 code like
        /// "en"; or C4Language.None to disable language-specific transformations like
        /// stemming; or C4Language.Default to fall back to the default language (as set by
        /// c4key_setDefaultFullTextLanguage.)</param>
        /// <param name="options">Query options. Only skip, limit, descending, rankFullText are used.</param>
        /// <param name="outError">On failure, error info will be stored here.</param>
        public static C4QueryEnumerator* c4view_fullTextQuery(C4View* view, C4Slice queryString,
            C4Slice queryStringLanguage, C4QueryOptions* options, C4Error* outError)
        {
#if DEBUG && !NET_3_5
            var retVal = _c4view_fullTextQuery(view, queryString, queryStringLanguage, options, outError);
            if(retVal != null) {
                _AllocatedObjects.TryAdd((IntPtr)retVal, "C4QueryEnumerator");
                #if TRACE
                Trace.WriteLine("[c4view_query] Allocated 0x{0}", ((IntPtr)retVal).ToString("X"));
#endif
            }

            return retVal;
#else
            return _c4view_fullTextQuery(view, queryString, queryStringLanguage, options, outError);
#endif
        }

        /// <summary>
        /// Runs a full-text query and returns an enumerator for the results.
        /// </summary>
        /// <returns>A new query enumerator. Fields are invalid until c4queryenum_next is called.</returns>
        /// <param name="view">The view to query.</param>
        /// <param name="queryString">A string containing the words to search for, separated by whitespace.</param>
        /// <param name="queryStringLanguage">The human language of the query string as an ISO-639 code like
        /// "en"; or C4Language.None to disable language-specific transformations like
        /// stemming; or C4Language.Default to fall back to the default language (as set by
        /// c4key_setDefaultFullTextLanguage.)</param>
        /// <param name="options">Query options. Only skip, limit, descending, rankFullText are used.</param>
        /// <param name="outError">On failure, error info will be stored here.</param>
        public static C4QueryEnumerator* c4view_fullTextQuery(C4View* view, string queryString,
            string queryStringLanguage, C4QueryOptions* options, C4Error* outError)
        {
            using(var queryString_ = new C4String(queryString))
            using(var queryStringLanguage_ = new C4String(queryStringLanguage)) {
                return c4view_fullTextQuery(view, queryString_.AsC4Slice(), queryStringLanguage_.AsC4Slice(),
                    options, outError);
            }
        }

        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi, EntryPoint = "c4view_geoQuery")]
        private static extern C4QueryEnumerator* _c4view_geoQuery(C4View* view, C4GeoArea area, C4Error* outError);

        /// <summary>
        /// Runs a geo-query and returns an enumerator for the results.
        /// </summary>
        /// <returns> A new query enumerator. Fields are invalid until c4queryenum_next is called.</returns>
        /// <param name="view">The view to query.</param>
        /// <param name="area">The bounding box to search for. Rows intersecting this will be returned.</param>
        /// <param name="outError">On failure, error info will be stored here.</param>
        public static C4QueryEnumerator* c4view_geoQuery(C4View* view, C4GeoArea area, C4Error* outError)
        {
#if DEBUG && !NET_3_5
            var retVal = _c4view_geoQuery(view, area, outError);
            if(retVal != null) {
                _AllocatedObjects.TryAdd((IntPtr)retVal, "C4QueryEnumerator");
                #if TRACE
                Trace.WriteLine("[c4view_query] Allocated 0x{0}", ((IntPtr)retVal).ToString("X"));
#endif
            }

            return retVal;
#else
            return _c4view_geoQuery(view, area, outError);
#endif
        }

        /// <summary>
        /// In a full-text query enumerator, returns the string that was emitted during indexing that
        /// contained the search term(s).
        /// </summary>
        /// <returns>The string that was emitted during indexing</returns>
        /// <param name="e">The enumerator to operate on</param>
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi, EntryPoint = "c4queryenum_fullTextMatched")]
        public static extern C4Slice _c4queryenum_fullTextMatched(C4QueryEnumerator* e);

        /// <summary>
        /// In a full-text query enumerator, returns the string that was emitted during indexing that
        /// contained the search term(s).
        /// </summary>
        /// <returns>The string that was emitted during indexing</returns>
        /// <param name="e">The enumerator to operate on</param>
        public static string c4queryenum_fullTextMatched(C4QueryEnumerator* e)
        {
            return BridgeSlice(() => _c4queryenum_fullTextMatched(e));
        }

        /// <summary>
        /// Given a document and the fullTextID from the enumerator, returns the text that was emitted
        /// during indexing.
        /// </summary>
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public static extern C4Slice c4view_fullTextMatched(C4View* view, C4Slice docID, ulong seq, uint fullTextID,
            C4Error* outError);

        /// <summary>
        /// Given a document and the fullTextID from the enumerator, returns the text that was emitted
        /// during indexing.
        /// </summary>
        public static string c4view_fullTextMatched(C4View* view, string docID, ulong seq, uint fullTextID,
            C4Error* outError)
        {
            using(var docID_ = new C4String(docID)) {
                return BridgeSlice(() => c4view_fullTextMatched(view, docID_.AsC4Slice(), seq, fullTextID, outError));
            }
        }

        /// <summary>
        /// Advances a query enumerator to the next row, populating its fields.
        /// Returns true on success, false at the end of enumeration or on error.
        /// </summary>
        /// <param name="e">The enumerator to operate on</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>true on success, false on error or end reached</returns>
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4queryenum_next(C4QueryEnumerator* e, C4Error* outError);

        /// <summary>
        /// Closes a query enumerator, any further access is invalid.
        /// </summary>
        /// <param name="e">The query enumerator to close</param>
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public static extern void c4queryenum_close(C4QueryEnumerator* e);

        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi, EntryPoint = "c4queryenum_free")]
        private static extern void _c4queryenum_free(C4QueryEnumerator* e);

        /// <summary>
        /// Frees a query enumerator.
        /// </summary>
        /// <param name="e">The enumerator to free</param>
        public static void c4queryenum_free(C4QueryEnumerator* e)
        {
#if DEBUG && !NET_3_5
            var ptr = (IntPtr)e;
            #if TRACE
            if(ptr != IntPtr.Zero && !_AllocatedObjects.ContainsKey(ptr)) {
                Trace.WriteLine("WARNING: [c4queryenum_free] freeing object 0x{0} that was not found in allocated list", ptr.ToString("X"));
            } else {
#endif
                _AllocatedObjects.TryRemove(ptr, out _Dummy);
            #if TRACE
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
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
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

        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi, EntryPoint = "c4kv_new")]
        private static extern C4KeyValueList* _c4kv_new();

        /// <summary>
        /// Creates a new key value list
        /// </summary>
        /// <returns>The created object</returns>
        public static C4KeyValueList* c4kv_new()
        {
#if DEBUG && !NET_3_5
            var retVal = _c4kv_new();
            if(retVal != null) {
                _AllocatedObjects.TryAdd((IntPtr)retVal, "C4KeyValueList");
                #if TRACE
                Trace.WriteLine("[c4view_query] Allocated 0x{0}", ((IntPtr)retVal).ToString("X"));
#endif
            }

            return retVal;
#else
            return _c4kv_new();
#endif
        }

        /// <summary>
        /// Adds a given key/value pair to the list
        /// </summary>
        /// <param name="kv">The list to add to</param>
        /// <param name="key">The key to add</param>
        /// <param name="value">The value to add</param>
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public static extern void c4kv_add(C4KeyValueList* kv, C4Key* key, C4Slice value);

        /// <summary>
        /// Adds a given key/value pair to the list
        /// </summary>
        /// <param name="kv">The list to add to</param>
        /// <param name="key">The key to add</param>
        /// <param name="value">The value to add</param>
        public static void c4kv_add(C4KeyValueList* kv, C4Key* key, string value)
        {
            using(var value_ = new C4String(value)) {
                c4kv_add(kv, key, value_.AsC4Slice());
            }
        }

        /// <summary>
        /// Removes all the keys and values from the list
        /// </summary>
        /// <param name="kv">The list to reset</param>
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public static extern void c4kv_reset(C4KeyValueList* kv);

        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi, EntryPoint = "c4kv_free")]
        private static extern void _c4kv_free(C4KeyValueList* kv);

        /// <summary>
        /// Frees the given key value list
        /// </summary>
        /// <param name="kv">The key value list to free</param>
        public static void c4kv_free(C4KeyValueList* kv)
        {
#if DEBUG && !NET_3_5
            var ptr = (IntPtr)kv;
                #if TRACE
            if(ptr != IntPtr.Zero && !_AllocatedObjects.ContainsKey(ptr)) {
                Trace.WriteLine("WARNING: [c4queryenum_free] freeing object 0x{0} that was not found in allocated list", ptr.ToString("X"));
            } else {
#endif
                _AllocatedObjects.TryRemove(ptr, out _Dummy);
            #if TRACE
            }
#endif
#endif
            _c4kv_free(kv);
        }

        /// <summary>
        /// Gets the error message associated with the given error
        /// </summary>
        /// <returns>The error message</returns>
        /// <param name="err">The error object to investigate</param>
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi, EntryPoint = "c4error_getMessage")]
        public static extern C4Slice _c4error_getMessage(C4Error err);

        /// <summary>
        /// Gets the error message associated with the given error
        /// </summary>
        /// <returns>The error message</returns>
        /// <param name="err">The error object to investigate</param>
        public static string c4error_getMessage(C4Error err)
        {
            return BridgeSlice(() => _c4error_getMessage(err));
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
            if(_LogCallback != null) {
                var sharpString = (string)message;
                _LogCallback(level, sharpString);
            }
        }
    }
}

