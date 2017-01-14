//
// LiteCoreDocEnumerator.cs
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
using System.Linq;
using System.Threading;
using LiteCore.Interop;
using C4SequenceNumber = System.UInt64;

namespace LiteCore
{
    public unsafe sealed class LiteCoreDocEnumerator : InteropObject
    {
        private long p_native;
        private C4DocEnumerator* _native
        {
            get {
                return (C4DocEnumerator*)p_native;
            }
            set {
                p_native = (long)value;
            }
        }

        internal LiteCoreDocEnumerator(C4Database* parent, string startDocID, string endDocID, C4EnumeratorOptions options)
        {
            _native = (C4DocEnumerator*)LiteCoreBridge.Check(err =>
            {
                var localOpts = options;
                return Native.c4db_enumerateAllDocs(parent, startDocID, endDocID, &localOpts, err);
            });
        }

        internal LiteCoreDocEnumerator(C4Database* parent, IEnumerable<string> docIDs, C4EnumeratorOptions options)
        {
            _native = (C4DocEnumerator*)LiteCoreBridge.Check(err =>
            {
                var localOpts = options;
                return Native.c4db_enumerateSomeDocs(parent, docIDs.ToArray(), &localOpts, err);
            });
        }

        internal LiteCoreDocEnumerator(C4Database* parent, C4SequenceNumber seq, C4EnumeratorOptions options)
        {
            _native = (C4DocEnumerator*)LiteCoreBridge.Check(err =>
            {
                var localOpts = options;
                return Native.c4db_enumerateChanges(parent, seq, &localOpts, err);
            });
        }

        protected override void Dispose(bool finalizing)
        {
            var native = (C4DocEnumerator *)Interlocked.Exchange(ref p_native, 0);
            if(native != null) {
                Native.c4enum_close(native);
                Native.c4enum_free(native);
            }
        }
    }
}
