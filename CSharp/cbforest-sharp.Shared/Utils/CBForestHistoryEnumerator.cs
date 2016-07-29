//
// CBForestHistoryEnumerator.cs
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

namespace CBForest
{
    /// <summary>
    /// An enumerator that iterates over a given document's history
    /// </summary>
    public sealed unsafe class CBForestHistoryEnumerator : IEnumerable<CBForestDocStatus>, IEnumerator<CBForestDocStatus>
    {

        #region Variables

        private C4Document *_doc;
        private CBForestDocStatus _current;
        private readonly bool _onlyLeaf;
        private readonly bool _byParent;
        private readonly bool _owner;

        #endregion

        #region Constructors

        /// <summary>
        /// Creates a CBForestHistoryEnumerator that enumerates all specified revisions
        /// of a document
        /// </summary>
        /// <param name="doc">The document to enumerate</param>
        /// <param name="onlyLeaf">If set to <c>true</c> only leaf revisions (i.e. revisions
        /// with no children) will be processed</param>
        /// <param name="owner">If set to <c>true</c>, the enumerator will free the document
        /// when finished</param>
        public CBForestHistoryEnumerator(C4Document *doc, bool onlyLeaf, bool owner)
        {
            _doc = doc;
            _onlyLeaf = onlyLeaf;
            _owner = owner;
            if (!_owner) {
                GC.SuppressFinalize(this);
            }
        }

        /// <summary>
        /// Creates a CBForestHistoryEnumerator that enumerates all specified revisions
        /// of a document
        /// </summary>
        /// <param name="db">The document to enumerate</param>
        /// <param name="startingSequence">The sequence to begin the enumeration from</param>
        /// <param name="onlyLeaf">If set to <c>true</c> only leaf revisions (i.e. revisions
        /// with no children) will be processed</param>
        public CBForestHistoryEnumerator(C4Database *db, long startingSequence, bool onlyLeaf)
        {
            _doc = Native.c4doc_getBySequence(db, (ulong)startingSequence, null);
            _onlyLeaf = onlyLeaf;
            _owner = true;
        }

        /// <summary>
        /// Creates a CBForestHistoryEnumerator that enumerates the the revision
        /// history of a document by parent (i.e. the "winning" chain)
        /// </summary>
        /// <param name="doc">The document to enumerate</param>
        /// <param name="owner">If set to <c>true</c>, the enumerator will free the document
        /// when finished</param>
        public CBForestHistoryEnumerator(C4Document* doc, bool owner)
            : this(doc, false, owner)
        {
            _byParent = true;
        }

        /// <summary>
        /// Finalizer
        /// </summary>
        ~CBForestHistoryEnumerator()
        {
            Dispose(true);
        }

        #endregion

        #region Private Methods

        private void Dispose(bool disposing)
        {
            if (!_owner) {
                return;
            }

            var doc = _doc;
            _doc = null;
            _current = null;
            if (doc != null) {
                Native.c4doc_free(doc);
            }
        }

        #endregion

        #region IEnumerator
        #pragma warning disable 1591

        public bool MoveNext()
        {
            if (_doc == null) {
                return false;
            }

            if (_current == null) {
                _current = new CBForestDocStatus(_doc, false);
                return true;
            }

            var retVal = false;
            if (_byParent) {
                retVal = Native.c4doc_selectParentRevision(_doc);
            } else if (_onlyLeaf) {
                retVal = Native.c4doc_selectNextLeafRevision(_doc, true, true, null);
            } else {
                retVal = Native.c4doc_selectNextRevision(_doc);
            }

            if (retVal) {
                _current = new CBForestDocStatus(_doc, false);
            }

            return retVal;
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

