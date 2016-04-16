//
// CBForestExpiryEnumerator.cs
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
using System.Runtime.InteropServices;

namespace CBForest
{
    public unsafe sealed class CBForestExpiryEnumerator : IEnumerable<string>
	{
        private readonly C4Database *_db;
        private readonly bool _dispose;

        public CBForestExpiryEnumerator(C4Database *db, bool dispose)
		{
            _db = db;
            _dispose = dispose;
		}

        #region IEnumerable

        public IEnumerator<string> GetEnumerator()
        {
            return new ExpiryEnumerator(_db, _dispose);
        }

        System.Collections.IEnumerator System.Collections.IEnumerable.GetEnumerator()
        {
            return GetEnumerator();
        }

        #endregion
	}

    internal unsafe sealed class ExpiryEnumerator : IEnumerator<string>
    {
        private readonly C4ExpiryEnumerator *_e;
        private readonly bool _dispose;
        private C4DocumentInfo *_currentInfo;

        public ExpiryEnumerator(C4Database *db, bool dispose)
        {
            _e = (C4ExpiryEnumerator*)RetryHandler.RetryIfBusy().Execute(err => Native.c4db_enumerateExpired(db, err));
            _dispose = dispose;
            _currentInfo = (C4DocumentInfo *)Marshal.AllocHGlobal(sizeof(C4DocumentInfo)).ToPointer();
        }

        ~ExpiryEnumerator()
        {
            Dispose(true);
        }

        private void Dispose(bool finalizing)
        {
            if (_dispose) {
                C4Error err;
                if (!Native.c4exp_purgeExpired(_e, &err)) {
                    Native._LogCallback(C4LogLevel.Error, String.Format("c4exp_purgeExpired failed: {0}", err));
                }
            }

            Native.c4exp_free(_e);
            Marshal.FreeHGlobal((IntPtr)_currentInfo);
        }

        #region IEnumerator

        public bool MoveNext()
        {
            if (_e == null) {
                return false;
            }

            var retVal = Native.c4exp_next(_e, null);
            if (retVal) {
                Native.c4exp_getInfo(_e, _currentInfo);
            }

            return retVal;
        }

        public void Reset()
        {
            throw new NotImplementedException();
        }

        object System.Collections.IEnumerator.Current
        {
            get
            {
                return Current;
            }
        }

        public string Current
        {
            get
            {
                return (string)_currentInfo->docID;
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
    }
}

