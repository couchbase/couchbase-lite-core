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
        private readonly C4Document *_doc;
        private CBForestDocStatus _current;
        private readonly bool _onlyLeaf;

        public CBForestHistoryEnumerator(C4Document *doc, bool onlyLeaf)
        {
            _doc = doc;
            _onlyLeaf = onlyLeaf;
        }

        ~CBForestHistoryEnumerator()
        {
            Dispose(true);
        }

        private void Dispose(bool disposing)
        {
            Native.c4doc_free(_doc);
        }

        public bool MoveNext()
        {
            var retVal = false;
            if (_onlyLeaf) {
                retVal = Native.c4doc_selectNextLeafRevision(_doc, true, true, null);
            } else {
                retVal = Native.c4doc_selectNextRevision(_doc);
            }

            if (retVal) {
                _current = new CBForestDocStatus(_doc);
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
            Dispose(false);
            GC.SuppressFinalize(this);
        }
    }
}

