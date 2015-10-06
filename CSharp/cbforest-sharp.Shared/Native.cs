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
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;

#if __IOS__
[assembly: ObjCRuntime.LinkWith("libCBForest-Interop.a", 
    ObjCRuntime.LinkTarget.Arm64 | ObjCRuntime.LinkTarget.ArmV7 | ObjCRuntime.LinkTarget.ArmV7s, ForceLoad=true,
    LinkerFlags="-lsqlite3 -lc++", Frameworks="", IsCxx=true)]
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

        /// <summary>
        /// Frees the memory of a C4Slice.
        /// </summary>
        /// <param name="slice">The slice that will have its memory freed</param>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        public static extern void c4slice_free(C4Slice slice);
        
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        private static extern C4Database* c4db_open(C4Slice path, C4DatabaseFlags flags, 
            C4EncryptionKey *encryptionKey, C4Error *outError);

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
        
        /// <summary>
        /// Closes the database and frees the object.
        /// </summary>
        /// <param name="db">The DB object to close</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>true on success, false otherwise</returns>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4db_close(C4Database *db, C4Error *outError);

        /// <summary>
        /// Closes the database, deletes the file, and frees the object
        /// </summary>
        /// <param name="db">The DB object to delete</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>true on success, false otherwise</returns>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4db_delete(C4Database *db, C4Error *outError);

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


        /// <summary>
        /// Frees the storage occupied by a raw document.
        /// </summary>
        /// <param name="rawDoc"></param>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        public static extern void c4raw_free(C4RawDocument *rawDoc);
        
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        private static extern C4RawDocument* c4raw_get(C4Database *db, C4Slice storeName, C4Slice docID, C4Error *outError);

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
        
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        private static extern bool c4raw_put(C4Database *db, C4Slice storeName, C4Slice key, C4Slice meta,
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

        /// <summary>
        /// Frees a C4Document.
        /// </summary>
        /// <param name="doc">The document to free</param>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        public static extern void c4doc_free(C4Document *doc);
        
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        private static extern C4Document* c4doc_get(C4Database *db, C4Slice docID, [MarshalAs(UnmanagedType.U1)]bool mustExist, 
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
        public static C4Document* c4doc_get(C4Database *db, string docID, bool mustExist, C4Error *outError)
        {
            using(var docID_ = new C4String(docID)) {
                return c4doc_get(db, docID_.AsC4Slice(), mustExist, outError);   
            }
        }
        
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi, EntryPoint="c4doc_getType")]
        private static extern C4Slice _c4doc_getType(C4Document *doc);

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
        
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        private static extern bool c4doc_selectRevision(C4Document *doc, C4Slice revID, [MarshalAs(UnmanagedType.U1)]bool withBody, 
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
        
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        private static extern byte c4doc_selectNextLeafRevision(C4Document *doc, byte includeDeleted, byte withBody, C4Error *outError);

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
        
        /// <summary>
        /// 
        /// </summary>
        /// <param name="e"></param>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        public static extern void c4enum_free(C4DocEnumerator *e);

        /// <summary>
        /// Creates an enumerator ordered by sequence.
        /// Caller is responsible for freeing the enumerator when finished with it.
        /// </summary>
        /// <param name="db">The database to operate on</param>
        /// <param name="since">The sequence number to start _after_.Pass 0 to start from the beginning.</param>
        /// <param name="options">Enumeration options (NULL for defaults).</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>A pointer to the enumeator on success, otherwise null</returns>
        [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public static extern C4DocEnumerator* c4db_enumerateChanges(C4Database* db, ulong since, C4ChangesOptions* options, C4Error* outError);

        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        private static extern C4DocEnumerator* c4db_enumerateAllDocs(C4Database *db, C4Slice startDocID, C4Slice endDocID, 
            C4AllDocsOptions *options, C4Error *outError);

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
        /// <returns>A pointer to the enumeator on success, otherwise null</returns>
        public static C4DocEnumerator* c4db_enumerateAllDocs(C4Database *db, string startDocID, string endDocID, C4AllDocsOptions *options,
            C4Error *outError)
        {
            using(var startDocID_ = new C4String(startDocID))
            using(var endDocID_ = new C4String(endDocID)) {
                return c4db_enumerateAllDocs(db, startDocID_.AsC4Slice(), endDocID_.AsC4Slice(), options, outError);
            }
        }

        /// <summary>
        /// Returns the next document from an enumerator, or NULL if there are no more.
        /// The caller is responsible for freeing the C4Document.
        /// Don't forget to free the enumerator itself when finished with it.
        /// </summary>
        /// <param name="e">The enumerator to operate on</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>A pointer to the document on success, otherwise null</returns>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        public static extern C4Document* c4enum_nextDocument(C4DocEnumerator *e, C4Error *outError);
        
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        private static extern bool c4doc_insertRevision(C4Document *doc, C4Slice revID, C4Slice body, 
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
        /// <returns>true on success, false otherwise</returns>
        public static bool c4doc_insertRevision(C4Document *doc, string revID, string body, bool deleted, bool hasAttachments,
            bool allowConflict, C4Error *outError)
        {
            using(var revID_ = new C4String(revID))
            using(var body_ = new C4String(body)) {
                return c4doc_insertRevision(doc, revID_.AsC4Slice(), body_.AsC4Slice(), deleted, 
                    hasAttachments, allowConflict, outError);
            }
        }
        
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        private static extern int c4doc_insertRevisionWithHistory(C4Document *doc, C4Slice revID, C4Slice body, 
            [MarshalAs(UnmanagedType.U1)]bool deleted, [MarshalAs(UnmanagedType.U1)]bool hasAttachments, C4Slice* history, 
            uint historyCount, C4Error *outError);

        /// <summary>
        ///  Adds a revision to a document, plus its ancestors (given in reverse chronological order.)
        /// On success, the new revision will be selected.
        /// Must be called within a transaction.
        /// <param name="doc">The document to operate on</param>
        /// <param name="revID">The ID of the revision being inserted</param>
        /// <param name="body">The (JSON) body of the revision</param>
        /// <param name="deleted">True if this revision is a deletion (tombstone)</param>
        /// <param name="hasAttachments">True if this revision contains an _attachments dictionary</param>
        /// <param name="history">The ancestors' revision IDs, starting with the parent, in reverse order</param>
        /// <param name="historyCount">The number of items in the history array</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>The number of revisions added to the document, or -1 on error.</returns>
        public static int c4doc_insertRevisionWithHistory(C4Document *doc, string revID, string body, bool deleted, 
            bool hasAttachments, string[] history, uint historyCount, C4Error *outError)
        {
            var flattenedStringArray = new C4String[history.Length + 2];
            flattenedStringArray[0] = new C4String(revID);
            flattenedStringArray[1] = new C4String(body);
            for(int i = 0; i < historyCount; i++) {
                flattenedStringArray[i + 2] = new C4String(history[i]);
            }
            
            var sliceArray = flattenedStringArray.Skip(2).Select<C4String, C4Slice>(x => x.AsC4Slice()).ToArray(); 
            var retVal = default(int);
            fixed(C4Slice *a = sliceArray) {
                retVal = c4doc_insertRevisionWithHistory(doc, flattenedStringArray[0].AsC4Slice(), 
                    flattenedStringArray[1].AsC4Slice(), deleted, hasAttachments, a, historyCount, outError);
            }
            
            foreach(var s in flattenedStringArray) {
                s.Dispose();   
            }
            
            return retVal;
        }
        
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        private static extern bool c4doc_setType(C4Document *doc, C4Slice docType, C4Error *outError);

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

        /// <summary>
        /// Creates a new empty C4Key.
        /// </summary>
        /// <returns>A pointer to a new empty C4Key</returns>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        public static extern C4Key* c4key_new();
        
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        private static extern C4Key* c4key_withBytes(C4Slice slice);

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

        /// <summary>
        /// Frees a C4Key.
        /// </summary>
        /// <param name="key">The key to free</param>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        public static extern void c4key_free(C4Key *key);

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
        
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        private static extern void c4key_addMapKey(C4Key *key, C4Slice s);

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
        
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi, EntryPoint="c4key_readString")]
        private static extern C4Slice _c4key_readString(C4KeyReader *reader);

        /// <summary>
        /// Reads a string value
        /// </summary>
        /// <param name="reader">The reader to operate on</param>
        /// <returns>The string value of the next token of the key</returns>
        public static string c4key_readString(C4KeyReader *reader)
        {
            return BridgeSlice(() => _c4key_readString(reader));
        }
        
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi, EntryPoint="c4key_toJSON")]
        private static extern C4Slice _c4key_toJSON(C4KeyReader *reader);

        /// <summary>
        /// Converts a C4KeyReader to JSON.
        /// </summary>
        /// <param name="reader">The reader to operate on</param>
        /// <returns>The JSON string result</returns>
        public static string c4key_toJSON(C4KeyReader *reader)
        {
            return BridgeSlice(() => _c4key_toJSON(reader));
        }
        
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        private static extern C4View* c4view_open(C4Database *db, C4Slice path, C4Slice viewName, C4Slice version, 
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
        public static C4View* c4view_open(C4Database *db, string path, string viewName, string version, C4DatabaseFlags flags,
            C4EncryptionKey *encryptionKey, C4Error *outError)
        {
            using(var path_ = new C4String(path))
            using(var viewName_ = new C4String(viewName))
            using(var version_ = new C4String(version)) {
                return c4view_open(db, path_.AsC4Slice(), viewName_.AsC4Slice(), version_.AsC4Slice(), flags, encryptionKey, outError);   
            }
        }

        /// <summary>
        /// Closes the view and frees the object.
        /// </summary>
        /// <param name="view">The view to close</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>true on success, false otherwise</returns>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4view_close(C4View *view, C4Error *outError);

        /// <summary>
        /// Erases the view index, but doesn't delete the database file.
        /// </summary>
        /// <param name="view">The view that will have its index erased</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>true on success, false otherwise</returns>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4view_eraseIndex(C4View *view, C4Error *outError);

        /// <summary>
        /// Deletes the database file and closes/frees the C4View.
        /// </summary>
        /// <param name="view">The view to operate on</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>true on success, false otherwise</returns>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4view_delete(C4View *view, C4Error *outError);

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

        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        private static extern C4Indexer* c4indexer_begin(C4Database *db, C4View** views, int viewCount, C4Error *outError);

        /// <summary>
        /// Creates an indexing task on one or more views in a database.
        /// </summary>
        /// <param name="db">The database to operate on</param>
        /// <param name="views">An array of views whose indexes should be updated in parallel.</param>
        /// <param name="viewCount">The number of views in the views[] array</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>A pointer to the indexer on success, otherwise null</returns>
        public static C4Indexer* c4indexer_begin(C4Database* db, C4View*[] views, int viewCount, C4Error* outError)
        {
            fixed(C4View** viewPtr = views) {
                return c4indexer_begin(db, viewPtr, viewCount, outError);
            }
        }

        /// <summary>
        /// Creates an enumerator that will return all the documents that need to be (re)indexed.
        /// </summary>
        /// <param name="indexer">The indexer to operate on</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>A pointer to the enumerator on success, otherwise null</returns>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        public static extern C4DocEnumerator* c4indexer_enumerateDocuments(C4Indexer *indexer, C4Error *outError);
        
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        private static extern bool c4indexer_emit(C4Indexer *indexer, C4Document *document, uint viewNumber, uint emitCount,
            C4Key** emittedKeys, C4Key** emittedValues, C4Error *outError);

        /// <summary>
        /// Emits new keys/values derived from one document, for one view.
        /// This function needs to be called once for each(document, view) pair.Even if the view's map
        /// function didn't emit anything, the old keys/values need to be cleaned up.
        /// </summary>
        /// <param name="indexer">The indexer to operate on</param>
        /// <param name="document">The document being indexed</param>
        /// <param name="viewNumber">The position of the view in the indexer's views[] array</param>
        /// <param name="emitCount">The number of emitted key/value pairs</param>
        /// <param name="emittedKeys">Array of keys being emitted</param>
        /// <param name="emittedValues"> Array of values being emitted</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>true on success, false otherwise</returns>
        public static bool c4indexer_emit(C4Indexer *indexer, C4Document *document, uint viewNumber, uint emitCount,
            C4Key*[] emittedKeys, C4Key*[] emittedValues, C4Error *outError)
        {
            fixed(C4Key** keysPtr = emittedKeys)
            fixed(C4Key** valuesPtr = emittedValues)
            {
                return c4indexer_emit(indexer, document, viewNumber, emitCount, keysPtr, valuesPtr, outError);
            }
        }

        /// <summary>
        /// Finishes an indexing task and frees the indexer reference.
        /// </summary>
        /// <param name="indexer">The indexer to operate on</param>
        /// <param name="commit">True to commit changes to the indexes, false to abort</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>true on success, false otherwise</returns>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4indexer_end(C4Indexer *indexer, bool commit, C4Error *outError);

        /// <summary>
        /// Runs a query and returns an enumerator for the results.
        /// The enumerator's fields are not valid until you call c4queryenum_next(), though.
        /// </summary>
        /// <param name="view">The view to operate on</param>
        /// <param name="options">The query options</param>
        /// <param name="outError">The error that occurred if the operation doesn't succeed</param>
        /// <returns>A pointer to the enumerator on success, otherwise null</returns>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        public static extern C4QueryEnumerator* c4view_query(C4View *view, C4QueryOptions *options, C4Error *outError);

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

        /// <summary>
        /// Frees a query enumerator.
        /// </summary>
        /// <param name="e">The enumerator to free</param>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        public static extern void c4queryenum_free(C4QueryEnumerator *e);
        
        /// <summary>
        /// Registers (or unregisters) a log callback, and sets the minimum log level to report.
        /// Before this is called, logs are by default written to stderr for warnings and errors.
        ///    Note that this setting is global to the entire process.
        /// </summary>
        /// <param name="level">The minimum level of message to log</param>
        /// <param name="callback">The logging callback, or NULL to disable logging entirely</param>
        [DllImport(DLL_NAME, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
        public static extern void c4log_register(C4LogLevel level, C4LogCallback callback);
        
        private static string BridgeSlice(Func<C4Slice> nativeFunc)
        {
            var rawRetVal = nativeFunc();
            var retVal = (string)rawRetVal;
            c4slice_free(rawRetVal);
            return retVal;
        }
    }
}

