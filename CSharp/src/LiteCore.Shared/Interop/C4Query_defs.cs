//
// C4Query_defs.cs
//
// Author:
// 	Jim Borden  <jim.borden@couchbase.com>
//
// Copyright (c) 2017 Couchbase, Inc All rights reserved.
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
using System.Linq;
using System.Runtime.InteropServices;
using System.Threading;

using LiteCore.Util;

namespace LiteCore.Interop
{
#if LITECORE_PACKAGED
    internal
#else
    public
#endif
    enum C4IndexType : uint
    {
        ValueIndex,
        FullTextIndex,
        GeoIndex,
    }

#if LITECORE_PACKAGED
    internal
#else
    public
#endif
    unsafe struct C4Query
    {
    }

#if LITECORE_PACKAGED
    internal
#else
    public
#endif
    unsafe partial struct C4QueryOptions
    {
        private byte _rankFullText;

        public bool rankFullText
        {
            get {
                return Convert.ToBoolean(_rankFullText);
            }
            set {
                _rankFullText = Convert.ToByte(value);
            }
        }
    }

#if LITECORE_PACKAGED
    internal
#else
    public
#endif
    unsafe partial struct C4IndexOptions
    {
        private IntPtr _language;
        private byte _ignoreDiacritics;

        public string language
        {
            get {
                return Marshal.PtrToStringAnsi(_language);
            }
            set {
                var old = Interlocked.Exchange(ref _language, Marshal.StringToHGlobalAnsi(value));
                Marshal.FreeHGlobal(old);
            }
        }

        public bool ignoreDiacritics
        {
            get {
                return Convert.ToBoolean(_ignoreDiacritics);
            }
            set {
                _ignoreDiacritics = Convert.ToByte(value);
            }
        }
    }

#if LITECORE_PACKAGED
    internal
#else
    public
#endif
    unsafe struct C4QueryEnumerator
    {
        public FLArrayIterator columns;
        public ulong fullTextID;
        public uint fullTextTermCount;
        public C4FullTextTerm* fullTextTerms;
    }

#if LITECORE_PACKAGED
    internal
#else
    public
#endif 
    struct C4FullTextTerm
    {
        public uint termIndex;
        public uint start, length;
    }
}