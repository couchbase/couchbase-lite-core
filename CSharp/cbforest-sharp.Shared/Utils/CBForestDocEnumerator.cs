//
// CBForestDocEnumerator.cs
//
// Author:
// 	Jim Borden  <jim.borden@couchbase.com>
//
// Copyright (c) 2015 Couchbase, Inc All rights reserved.
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
using System.Collections;
using System.Collections.Generic;
using System.Threading;

namespace CBForest
{
    /// <summary>
    /// A class the represents an entry in a document enumerator
    /// </summary>
    public unsafe sealed class CBForestDocStatus : IDisposable
    {

        #region Variables

        /// <summary>
        /// The current native document object
        /// </summary>
        public readonly C4Document *Document;

        public readonly long Sequence;
        private readonly bool _owner;
        private string _docID;
        private string _revID;

        #endregion

        #region Properties

        /// <summary>
        /// Gets the document ID of the current document
        /// </summary>
        public string CurrentDocID 
        { 
            get { 
                if (_docID == null) {
                    _docID = (string)Document->docID;
                }

                return _docID;  
            } 
        }

        /// <summary>
        /// Gets the revision ID of the current document revision
        /// </summary>
        public string CurrentRevID 
        { 
            get { 
                if (_revID == null) {
                    _revID = (string)Document->revID;
                }

                return _revID;  
            } 
        }

        /// <summary>
        /// Gets whether or not the current document revision has a body
        /// </summary>
        public bool HasRevisionBody { get { return Native.c4doc_hasRevisionBody(Document);  } }

        /// <summary>
        /// Gets whether or not the current document has any revisions
        /// </summary>
        public bool Exists { get { return Document->Exists; } }

        /// <summary>
        /// Gets whether or not the current document is conflicted
        /// </summary>
        public bool IsConflicted { get { return Document->IsConflicted; } }

        /// <summary>
        /// Gets whether or not the current document is deleted
        /// </summary>
        public bool IsDeleted { get { return Document->IsDeleted; } }

        /// <summary>
        /// Gets the current document revision
        /// </summary>
        public C4Revision SelectedRev { get { return Document->selectedRev; } }

        #endregion

        #region Constructors

        /// <summary>
        /// Constructor
        /// </summary>
        /// <param name="doc">The document to retrieve information form.</param>
        /// <param name="owner">Whether or not the instance should own the
        /// document (i.e. free it when it is finished)</param>
        public CBForestDocStatus(C4Document *doc, bool owner)
        {
            Document = doc;
            _owner = owner;
            Sequence = (long)doc->sequence;
            if (!_owner) {
                GC.SuppressFinalize(this);
            }
        }

        ~CBForestDocStatus()
        {
            Dispose(true);
        }

        #endregion

        #region Private Methods

        private void Dispose(bool disposing)
        {
            Native.c4doc_free(Document);
            GC.SuppressFinalize(this);
        }

        #endregion

        #region IDisposable
        #pragma warning disable 1591

        public void Dispose()
        {
            if (!_owner) {
                return;
            }

            Dispose(false);
        }

        #pragma warning restore 1591
        #endregion

    }

    /// <summary>
    /// An enumerator that iterates over a given set of documents
    /// (that "set" could also mean all documents)
    /// </summary>
    public unsafe sealed class CBForestDocEnumerator : IEnumerable<CBForestDocStatus>, IEnumerator<CBForestDocStatus>
    {

        #region Variables

        private readonly C4DocEnumerator *_e;
        private CBForestDocStatus _current;
        private delegate bool DocValidationDelegate(C4Document *doc);
        private DocValidationDelegate _validationLogic;

        #endregion

        #region Constructors

        /// <summary>
        /// Constructor for enumerating over a set of documents
        /// </summary>
        /// <param name="db">The database to retrieve documents from</param>
        /// <param name="keys">The keys to retrieve.</param>
        /// <param name="options">The enumeration options (null for default).</param>
        public CBForestDocEnumerator(C4Database *db, string[] keys, C4EnumeratorOptions options)
        {
            var options_ = &options;
            _e = (C4DocEnumerator *)RetryHandler.RetryIfBusy().Execute(err => Native.c4db_enumerateSomeDocs(db, keys, options_, err));
        }

        /// <summary>
        /// Constructor for enumerating over a given subset of documents in a database
        /// </summary>
        /// <param name="db">The database to retrieve documents from</param>
        /// <param name="startKey">The key to start enumeration from</param>
        /// <param name="endKey">The key to end enumeration at</param>
        /// <param name="options">The enumeration options (null for default).</param>
        public CBForestDocEnumerator(C4Database *db, string startKey, string endKey, C4EnumeratorOptions options)
        {
            var options_ = &options;
            _e = (C4DocEnumerator *)RetryHandler.RetryIfBusy().Execute(err => Native.c4db_enumerateAllDocs(db, startKey, endKey, options_, err));
        }

        /// <summary>
        /// Constructor for enumerating over documents which need indexing
        /// by a given indexer
        /// </summary>
        /// <param name="indexer">The indexer to use for enumeration.</param>
        public CBForestDocEnumerator(C4Indexer *indexer)
        {
            _e = (C4DocEnumerator *)RetryHandler.RetryIfBusy().AllowError(0, C4ErrorDomain.ForestDB)
                .Execute(err => Native.c4indexer_enumerateDocuments(indexer, err));
            _validationLogic = doc => !((string)doc->docID).StartsWith("_design/");
        }

        /// <summary>
        /// Constructor for enumerating over all documents starting from a given sequence
        /// </summary>
        /// <param name="db">The database to retrieve documents from</param>
        /// <param name="lastSequence">The sequence to start enumerating from</param>
        /// <param name="options">The enumeration options (null for default).</param>
        public CBForestDocEnumerator(C4Database *db, long lastSequence, C4EnumeratorOptions options)
        {
            var options_ = &options;
            _e = (C4DocEnumerator *)RetryHandler.RetryIfBusy().Execute(err => Native.c4db_enumerateChanges(db, (ulong)lastSequence, options_, err));
        }

        ~CBForestDocEnumerator()
        {
            Dispose(true);
        }

        #endregion

        #region Private Methods

        private void Dispose(bool disposing)
        {
            if (!disposing && _current != null) {
                _current.Dispose();
            }

            Native.c4enum_free(_e);
        }

        #endregion

        #region IEnumerator
        #pragma warning disable 1591

        public bool MoveNext()
        {
            if (_current != null) {
                _current.Dispose();
                _current = null;
            }

            if(_e == null) {
                return false;
            }

            var docPtr = default(C4Document*);
            do {
                Native.c4doc_free(docPtr);
                docPtr = Native.c4enum_nextDocument(_e, null);
                if (docPtr == null) {
                    return false;
                }
            } while(_validationLogic != null && !_validationLogic(docPtr));

            _current = new CBForestDocStatus(docPtr, true);
            return true;
        }

        public void Reset()
        {
            throw new NotSupportedException();
        }

        public CBForestDocStatus Current
        {
            get {
                return _current;
            }
        }

        object IEnumerator.Current
        {
            get
            {
                return Current;
            }
        }

        #endregion

        #region IEnumerable

        public IEnumerator<CBForestDocStatus> GetEnumerator()
        {
            return this;
        }

        IEnumerator IEnumerable.GetEnumerator()
        {
            return GetEnumerator();
        }

        #endregion

        #region IDisposable

        public void Dispose()
        {
            Dispose(false);
            GC.SuppressFinalize(this);
        }

        #pragma warning restore 1591
        #endregion
    }
}

