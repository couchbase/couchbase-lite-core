//
// C4String.cs
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
using System.Text;

using LiteCore.Interop;

namespace LiteCore.Util
{
    /// <summary>
    /// Helper class for marshalling string &lt;&gt; C4Slice without creating an extra copy
    /// of the bytes.  Not for storage or long-term use
    /// </summary>
    public struct C4String : IDisposable
    {
        private GCHandle _handle; // Stores the UTF-8 bytes in a pinned location

        /// <summary>
        /// Constructor
        /// </summary>
        /// <param name="s">The string to store in this instance</param>
        public C4String(string s)
        {
            _handle = new GCHandle();
            if(s != null) {
                var bytes = Encoding.UTF8.GetBytes(s);
                _handle = GCHandle.Alloc(bytes, GCHandleType.Pinned);
            }
        }

        /// <summary>
        /// Returns this object as a C4Slice.  This object will only be valid
        /// while the original C4String object is valid.
        /// </summary>
        /// <returns>Ths C4String instance as a C4Slice</returns>
        public unsafe C4Slice AsC4Slice()
        {
            if(!_handle.IsAllocated || _handle.Target == null) {
                return C4Slice.Null;
            }

            var bytes = _handle.Target as byte[];
            return new C4Slice(_handle.AddrOfPinnedObject().ToPointer(), (uint)bytes.Length);
        }

#pragma warning disable 1591
        public void Dispose()
        {
            if(_handle.IsAllocated) {
                _handle.Free();
            }
        }
#pragma warning restore 1591
    }

    internal unsafe struct C4SliceEnumerator : IEnumerator<byte>
    {
        private readonly byte* _start;
        private byte* _current;
        private readonly int _length;

        public C4SliceEnumerator(void* buf, int length)
        {
            _start = (byte*)buf;
            _current = _start - 1;
            _length = length;
        }

        public bool MoveNext()
        {
            if((_current - _start) >= _length - 1) {
                return false;
            }

            _current++;
            return true;
        }

        public void Reset()
        {
            _current = _start;
        }

        object System.Collections.IEnumerator.Current
        {
            get {
                return Current;
            }
        }

        public void Dispose()
        {
            // No-op
        }

        public byte Current
        {
            get {
                return *_current;
            }
        }
    }
}
