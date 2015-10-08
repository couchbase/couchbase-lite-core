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
    public sealed class CBForestQueryStatus
    {
        private readonly Lazy<string> _docID;
        private readonly Lazy<string> _keyJSON;
        private readonly Lazy<string> _valueJSON;

        public readonly C4KeyReader Key;

        public readonly C4KeyReader Value;

        public string DocID
        {
            get {
                return _docID.Value;
            }
        }

        public string KeyJSON
        {
            get {
                return _keyJSON.Value;
            }
        }

        public string ValueJSON
        {
            get {
                return _valueJSON.Value;
            }
        }

        internal unsafe CBForestQueryStatus(C4Slice docID, C4KeyReader key, C4KeyReader value)
        {
            _docID = new Lazy<string>(() => (string)docID);
            Key = key;
            Value = value;
            _keyJSON = new Lazy<string>(() =>
            {
                var localKey = key;
                return Native.c4key_toJSON(&localKey);
            });

            _valueJSON = new Lazy<string>(() =>
            {
                var localVal = value;
                return Native.c4key_toJSON(&localVal);
            });
        }
    }

    public unsafe sealed class CBForestQueryEnumerator : IEnumerator<CBForestQueryStatus>, IEnumerable<CBForestQueryStatus>
    {
        private readonly C4QueryEnumerator *_e;
        private CBForestQueryStatus _current;

        public CBForestQueryEnumerator(C4QueryEnumerator *e)
        {
            _e = e;
        }

        ~CBForestQueryEnumerator()
        {
            Dispose(true);
        }

        private void Dispose(bool disposing)
        {
            Native.c4queryenum_free(_e);
        }

        #region IEnumerator implementation

        public bool MoveNext()
        {
            var retVal = Native.c4queryenum_next(_e, null);
            if (retVal) {
                _current = new CBForestQueryStatus(_e->docID, _e->key, _e->value);
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

        #endregion
    }
}

