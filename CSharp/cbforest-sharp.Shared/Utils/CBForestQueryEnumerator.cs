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
    public unsafe sealed class CBForestQueryStatus
    {

        #region Variables

        private C4Slice _docIDSlice;
        private string _docID;
        private string _keyJSON;
        private string _valueJSON;
        private string _geoJSON;
        private C4FullTextTerm *_fullTextTerms;

        /// <summary>
        /// The key of this entry
        /// </summary>
        public readonly C4KeyReader Key;

        /// <summary>
        /// The value of this entry
        /// </summary>
        public readonly C4Slice Value;

        /// <summary>
        /// Gets the geoquery JSON information as it exists in the store
        /// </summary>
        public readonly C4Slice GeoJSONRaw;

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

        /// <summary>
        /// Gets the geoquery information in JSON format
        /// </summary>
        public string GeoJSON
        {
            get { 
                if(_geoJSON == null) {
                    _geoJSON = (string)GeoJSONRaw;  
                }
                
                return _geoJSON;
            }
        }

        /// <summary>
        /// Gets the count of the full text terms on this query, if applicable
        /// </summary>
        public uint FullTextTermCount { get; private set; }

        /// <summary>
        /// Gets the bounding box for the geoquery of this query, if applicable
        /// </summary>
        /// <value>The bounding box.</value>
        public C4GeoArea BoundingBox { get; private set; }

        #endregion

        #region Constructors

        internal unsafe CBForestQueryStatus(C4QueryEnumerator *e)
        {
            Key = e->key;
            Value = e->value;
            DocSequence = (long)e->docSequence;
            _docIDSlice = e->docID;
            _fullTextTerms = e->fullTextTerms;
            FullTextTermCount = e->fullTextTermCount;
            BoundingBox = e->geoBBox;
            GeoJSONRaw = e->geoJSON;
        }

        #endregion
        
        #region Public Methods

        /// <summary>
        /// Gets the full text term at the specified index
        /// </summary>
        /// <returns>The full text term at the specified index.</returns>
        /// <param name="index">The index to check.</param>
        public C4FullTextTerm GetFullTextTerm(int index)
        {
            return _fullTextTerms[index]; 
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

        /// <summary>
        /// Finalizer
        /// </summary>
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
            if (_e == null) {
                return false;
            }

            var err = new C4Error();
            var retVal = Native.c4queryenum_next(_e, &err);
            if (retVal) {
                _current = new CBForestQueryStatus(_e);
            } else {
                if (err.code != (int)ForestDBStatus.Success) {
                    throw new CBForestException(err);
                }
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

