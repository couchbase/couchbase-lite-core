//
// LiteCoreDocument.cs
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
using LiteCore.Interop;
using C4SequenceNumber = System.UInt64;

namespace LiteCore
{
    public unsafe sealed class LiteCoreDocument : InteropObject
    {
        private C4Document* _native;

        public string DocType
        {
            get {
                return Native.c4doc_getType(_native);
            }
            set {
                Native.c4doc_setType(_native, value);
            }
        }

        internal static LiteCoreDocument Get(C4Database* parent, string docID, bool mustExist)
        {
            C4Error err;
            C4Document* native = Native.c4doc_get(parent, docID, mustExist, &err);
            if(native == null) {
                if(err.Code != 0) {
                    throw new LiteCoreException(err);
                }

                return null;
            }

            return new LiteCoreDocument(native);
        }

        internal static LiteCoreDocument Get(C4Database* parent, C4SequenceNumber seq)
        {
            C4Error err;
            C4Document* native = Native.c4doc_getBySequence(parent, seq, &err);
            if(native == null) {
                if(err.Code != 0) {
                    throw new LiteCoreException(err);
                }

                return null;
            }

            return new LiteCoreDocument(native);
        }

        public void Save(uint maxRevTreeDepth)
        {
            LiteCoreBridge.Check(err => Native.c4doc_save(_native, maxRevTreeDepth, err));
        }

        private LiteCoreDocument(C4Document* native)
        {
            _native = native;
        }

        protected override void Dispose(bool finalizing)
        {
            var native = _native;
            _native = null;
            if(native != null) {
                Native.c4doc_free(native);
            }
        }
    }
}
