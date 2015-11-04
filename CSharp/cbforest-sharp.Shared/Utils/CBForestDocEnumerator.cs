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
        private readonly bool _owner;
        private Lazy<string> _docID;
        private Lazy<string> _revID;

        public string CurrentDocID { get { return _docID.Value;  } }

        public string CurrentRevID { get { return _revID.Value;  } }

        public bool HasRevisionBody { get { return Native.c4doc_hasRevisionBody(Document);  } }

        public bool Exists { get { return Document->Exists; } }

        public bool IsConflicted { get { return Document->IsConflicted; } }

        public bool IsDeleted { get { return Document->IsDeleted; } }

        public C4Document.rev SelectedRev { get { return Document->selectedRev; } }

        public CBForestDocStatus(C4Document *doc, bool owner)
        {
            Document = doc;
            _owner = owner;
            if (!_owner) {
                GC.SuppressFinalize(this);
            }

            _docID = new Lazy<string>(() => (string)Document->docID);
            _revID = new Lazy<string>(() => (string)Document->revID);
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
            if (!_owner) {
                return;
            }

            Dispose(false);
        }
    }

    public unsafe sealed class CBForestDocEnumerator : IEnumerable<CBForestDocStatus>, IEnumerator<CBForestDocStatus>
    {
        private readonly C4DocEnumerator *_e;
        private CBForestDocStatus _current;
        private delegate bool DocValidationDelegate(C4Document *doc);
        private DocValidationDelegate _validationLogic;

        public CBForestDocEnumerator(C4Database *db, string[] keys, C4EnumeratorOptions options)
        {
            var options_ = &options;
            _e = (C4DocEnumerator *)RetryHandler.RetryIfBusy().Execute(err => Native.c4db_enumerateSomeDocs(db, keys, options_, err));
        }

        public CBForestDocEnumerator(C4Database *db, string startKey, string endKey, C4EnumeratorOptions options)
        {
            var options_ = &options;
            _e = (C4DocEnumerator *)RetryHandler.RetryIfBusy().Execute(err => Native.c4db_enumerateAllDocs(db, startKey, endKey, options_, err));
        }

        public CBForestDocEnumerator(C4Indexer *indexer)
        {
            _e = (C4DocEnumerator *)RetryHandler.RetryIfBusy().AllowError(0, C4ErrorDomain.ForestDB)
                .Execute(err => Native.c4indexer_enumerateDocuments(indexer, err));
            _validationLogic = doc => !((string)doc->docID).StartsWith("_design/");
        }

        public CBForestDocEnumerator(C4Database *db, long lastSequence, C4EnumeratorOptions options)
        {
            var options_ = &options;
            _e = (C4DocEnumerator *)RetryHandler.RetryIfBusy().Execute(err => Native.c4db_enumerateChanges(db, (ulong)lastSequence, options_, err));
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

