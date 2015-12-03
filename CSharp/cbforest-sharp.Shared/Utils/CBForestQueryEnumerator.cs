//
// CBForestQueryEnumerator.cs
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
using System.Collections.Generic;

namespace CBForest
{
    /// <summary>
    /// This class provides information about the current entry in a
    /// CBForestQueryEnumerator
    /// </summary>
    public sealed class CBForestQueryStatus
    {

        #region Variables

        private C4Slice _docIDSlice;
        private string _docID;
        private string _keyJSON;
        private string _valueJSON;

        /// <summary>
        /// The key of this entry
        /// </summary>
        public readonly C4KeyReader Key;

        /// <summary>
        /// The value of this entry
        /// </summary>
        public readonly C4Slice Value;

        /// <summary>
        /// The sequence number of the document that
        /// generated this entry
        /// </summary>
        public readonly long DocSequence;

        #endregion

        #region Properties

        /// <summary>
        /// Gets the document ID of the document
        /// that generated this entry
        /// </summary>
        public string DocID
        {
            get {
                if (_docID == null) {
                    _docID = (string)_docIDSlice;
                }

                return _docID;
            }
        }

        /// <summary>
        /// Gets the key of this entry
        /// in JSON format
        /// </summary>
        public unsafe string KeyJSON
        {
            get {
                var localKey = Key;
                if (_keyJSON == null) {
                    _keyJSON = Native.c4key_toJSON(&localKey);
                }

                return _keyJSON;
            }
        }

        /// <summary>
        /// Gets the value of this entry
        /// in JSON format
        /// </summary>
        public string ValueJSON
        {
            get {
                if (_valueJSON == null) {
                    _valueJSON = (string)Value;
                }

                return _valueJSON;
            }
        }

        #endregion

        #region Constructors

        internal unsafe CBForestQueryStatus(C4Slice docID, C4KeyReader key, C4Slice value, long docSequence)
        {
            Key = key;
            Value = value;
            DocSequence = docSequence;
            _docIDSlice = docID;
        }

        #endregion
    }

    /// <summary>
    /// An enumerator that iterates over query results
    /// </summary>
    public unsafe sealed class CBForestQueryEnumerator : IEnumerator<CBForestQueryStatus>, IEnumerable<CBForestQueryStatus>
    {

        #region Variables

        private readonly C4QueryEnumerator *_e;
        private CBForestQueryStatus _current;

        #endregion

        #region Constructors

        /// <summary>
        /// Constructor
        /// </summary>
        /// <param name="e">The native query enumerator object to use</param>
        public CBForestQueryEnumerator(C4QueryEnumerator *e)
        {
            _e = e;
        }

        ~CBForestQueryEnumerator()
        {
            Dispose(true);
        }

        #endregion

        #region Private Methods

        private void Dispose(bool disposing)
        {
            Native.c4queryenum_free(_e);
        }

        #endregion

        #region IEnumerator implementation
        #pragma warning disable 1591

        public bool MoveNext()
        {
            var retVal = Native.c4queryenum_next(_e, null);
            if (retVal) {
                _current = new CBForestQueryStatus(_e->docID, _e->key, _e->value, (long)_e->docSequence);
            }

            return retVal;
        }

        public void Reset()
        {
            throw new NotSupportedException();
        }

        object System.Collections.IEnumerator.Current
        {
            get
            {
                return Current;
            }
        }

        public CBForestQueryStatus Current
        {
            get
            {
                return _current;
            }
        }

        #endregion

        #region IDisposable implementation

        public void Dispose()
        {
            Dispose(false);
            GC.SuppressFinalize(this);
        }

        #endregion

        #region IEnumerable implementation

        public IEnumerator<CBForestQueryStatus> GetEnumerator()
        {
            return this;
        }

        System.Collections.IEnumerator System.Collections.IEnumerable.GetEnumerator()
        {
            return GetEnumerator();
        }

        #pragma warning restore 1591
        #endregion
    }
}

