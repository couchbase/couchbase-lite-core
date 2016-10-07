//
// Database.cs
//
// Author:
// 	Jim Borden  <jim.borden@couchbase.com>
//
// Copyright (c) 2016 Couchbase, Inc All rights reserved.
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

using LiteCore.Interop;
using C4SequenceNumber = System.UInt64;

namespace LiteCore
{
    [Flags]
    public enum DatabaseFlags : uint
    {
        Create = 1,
        ReadOnly = 2,
        AutoCompact = 4,
        Bundled = 8
    }

    public unsafe sealed class LiteCoreDatabase : InteropObject
    {
        private C4Database* _native;

        public string Path
        {
            get {
                return Native.c4db_getPath(_native);
            }
        }

        public bool IsCompacting
        {
            get {
                return Native.c4db_isCompacting(_native);
            }
        }

        public bool InTransaction
        {
            get {
                return Native.c4db_isInTransaction(_native);
            }
        }

        public C4DatabaseConfig* Config
        {
            get {
                return Native.c4db_getConfig(_native);
            }
        }

        public LiteCoreDatabase(string path, C4DatabaseConfig config)
        {
            _native = (C4Database *)LiteCoreBridge.Check(err =>
            {
                var localConfig = config;
                return Native.c4db_open(path, &localConfig, err);
            });
        }

        public static void Delete(string path, C4DatabaseConfig config)
        {
            LiteCoreBridge.Check(err =>
            {
                var localConfig = config;
                return Native.c4db_deleteAtPath(path, &localConfig, err);
            });
        }

        public C4SequenceNumber GetLastSequence()
        {
            return Native.c4db_getLastSequence(_native);
        }

        public ulong GetDocumentCount()
        {
            return Native.c4db_getDocumentCount(_native);
        }

        public ulong GetNextDocExpiration()
        {
            return Native.c4db_nextDocExpiration(_native);
        }

        public ulong GetDocExpiration(string docID)
        {
            return Native.c4doc_getExpiration(_native, docID);
        }

        public void Close()
        {
            LiteCoreBridge.Check(err => Native.c4db_close(_native, err));
        }

        public void Delete()
        {
            LiteCoreBridge.Check(err => Native.c4db_delete(_native, err));
        }

        public void Rekey(C4EncryptionKey* newKey)
        {
            LiteCoreBridge.Check(err => Native.c4db_rekey(_native, newKey, err));
        }

        public void BeginTransaction()
        {
            LiteCoreBridge.Check(err => Native.c4db_beginTransaction(_native, err));
        }

        public void EndTransaction(bool commit)
        {
            LiteCoreBridge.Check(err => Native.c4db_endTransaction(_native, commit, err));
        }

        public LiteCoreDocument GetDocument(string docID, bool mustExist)
        {
            return LiteCoreDocument.Get(_native, docID, mustExist);
        }

        public LiteCoreDocument GetDocument(C4SequenceNumber seq)
        {
            return LiteCoreDocument.Get(_native, seq);
        }

        public LiteCoreDocEnumerator GetEnumerator(string startDocID, string endDocID, C4EnumeratorOptions options)
        {
            return new LiteCoreDocEnumerator(_native, startDocID, endDocID, options);
        }

        public LiteCoreDocEnumerator GetEnumerator(IEnumerable<string> docIDs, C4EnumeratorOptions options)
        {
            return new LiteCoreDocEnumerator(_native, docIDs, options);
        }

        public LiteCoreDocEnumerator GetChangesSince(C4SequenceNumber seq, C4EnumeratorOptions options)
        {
            return new LiteCoreDocEnumerator(_native, seq, options);
        }

        public LiteCoreExpiryEnumerable GetExpired()
        {
            return new LiteCoreExpiryEnumerable(_native);
        }

        public void Compact()
        {
            LiteCoreBridge.Check(err => Native.c4db_compact(_native, err));
        }

        public void Purge(string docID)
        {
            LiteCoreBridge.Check(err => Native.c4db_purgeDoc(_native, docID, err));
        }

        protected override void Dispose(bool finalizing)
        {
            var native = _native;
            _native = null;
            if(native != null) {
                Native.c4db_free(native);
            }
        }
    }
}
