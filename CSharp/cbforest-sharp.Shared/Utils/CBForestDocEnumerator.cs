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

namespace CBForest
{
    public unsafe sealed class CBForestDocStatus : IDisposable
    {
        public readonly C4Document *Document;

        public CBForestDocStatus(C4Document *doc)
        {
            Document = doc;
        }

        ~CBForestDocStatus()
        {
            Dispose(true);
        }

        private void Dispose(bool disposing)
        {
            Native.c4doc_free(Document);
            GC.SuppressFinalize(this);
        }

        public void Dispose()
        {
            Dispose(false);
        }
    }

    public unsafe sealed class CBForestDocEnumerator : IEnumerable<CBForestDocStatus>, IEnumerator<CBForestDocStatus>
    {
        private readonly C4DocEnumerator *_e;
        private CBForestDocStatus _current;

        public CBForestDocEnumerator(C4Database *db, string startKey, string endKey, C4AllDocsOptions options)
        {
            var err = default(C4Error);
            _e = Native.c4db_enumerateAllDocs(db, startKey, endKey, &options, &err);
            if (_e == null) {
                throw new CBForestException(err.code, err.domain);
            }
        }

        public CBForestDocEnumerator(C4Indexer *indexer)
        {
            var err = default(C4Error);
            _e = Native.c4indexer_enumerateDocuments(indexer, &err);
            if (_e == null) {
                throw new CBForestException(err.code, err.domain);
            }
        }

        ~CBForestDocEnumerator()
        {
            Dispose(true);
        }

        private void Dispose(bool disposing)
        {
            if (!disposing && _current != null) {
                _current.Dispose();
            }

            Native.c4enum_free(_e);
        }

        public bool MoveNext()
        {
            if (_current != null) {
                _current.Dispose();
                _current = null;
            }

            var docPtr = Native.c4enum_nextDocument(_e, null);
            if (docPtr == null) {
                return false;
            }

            _current = new CBForestDocStatus(docPtr);
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

