//
// LiteCoreExpiryEnumerator.cs
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
using System.Threading;
using LiteCore.Interop;

namespace LiteCore
{
#if LITECORE_PACKAGED
    internal
#else
    public
#endif
         unsafe sealed class LiteCoreExpiryEnumerable : InteropObject, IEnumerable<string>
    {
        private long _native;
        private C4ExpiryEnumerator* Native
        {
            get {
                return (C4ExpiryEnumerator*)_native;
            }
            set {
                _native = (long)value;
            }
        }

        public LiteCoreExpiryEnumerable(C4Database* parent)
        {
            Native = (C4ExpiryEnumerator*)LiteCoreBridge.Check(err => Interop.Native.c4db_enumerateExpired(parent, err));
        }

        protected override void Dispose(bool finalizing)
        {
            var native = (C4ExpiryEnumerator*)Interlocked.Exchange(ref _native, 0);
            if(native != null) {
                Interop.Native.c4exp_close(native);
                Interop.Native.c4exp_free(native);
            }
        }

        public IEnumerator<string> GetEnumerator()
        {
            return new LiteCoreExpiryEnumerator(Native);
        }

        System.Collections.IEnumerator System.Collections.IEnumerable.GetEnumerator()
        {
            return GetEnumerator();
        }

        private sealed class LiteCoreExpiryEnumerator : IEnumerator<string>
        {
            #region Variables

            private readonly C4ExpiryEnumerator* _native;

            #endregion

            #region Properties

            public string Current
            {
                get {
                    return Interop.Native.c4exp_getDocID(_native);
                }
            }

            object System.Collections.IEnumerator.Current
            {
                get {
                    return Current;
                }
            }

            #endregion

            #region Constructors

            public LiteCoreExpiryEnumerator(C4ExpiryEnumerator* native)
            {
                _native = native;
            }

            #endregion

            #region IDisposable

            public void Dispose() {}

            #endregion

            #region IEnumerator

            public bool MoveNext()
            {
                return Interop.Native.c4exp_next(_native, null);
            }

            public void Reset()
            {
                throw new NotSupportedException();
            }

            #endregion
        }
    }
}
