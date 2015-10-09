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
    public sealed unsafe class CBForestHistoryEnumerator : IEnumerable<CBForestDocStatus>, IEnumerator<CBForestDocStatus>
    {

        #region Variables

        private readonly C4Document *_doc;
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
            var err = default(C4Error);
            var selectedCurrent = Native.c4doc_selectCurrentRevision(doc);
            if (!selectedCurrent) {
                throw new CBForestException(err.code, err.domain);
            }

            _doc = doc;
            _onlyLeaf = onlyLeaf;
            _owner = owner;
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

        #endregion

        public bool MoveNext()
        {
            if (_current == null) {
                _current = new CBForestDocStatus(_doc, _owner);
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
                _current.Dispose();
                _current = new CBForestDocStatus(_doc, _owner);
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

        public IEnumerator<CBForestDocStatus> GetEnumerator()
        {
            return this;
        }

        IEnumerator IEnumerable.GetEnumerator()
        {
            return GetEnumerator();
        }

        public void Dispose()
        {
            if (_current != null) {
                _current.Dispose();
                _current = null;
            }
        }
    }
}

