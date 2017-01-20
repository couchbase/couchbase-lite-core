//
// C4Key_defs.cs
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
    public enum C4KeyToken : byte
    {
        Null,
        Bool,
        Number,
        String,
        Array,
        Map,
        EndSequence,
        Special,
        Error = 255
    }

    public unsafe struct C4KeyReader
    {
        public void* bytes;
        private UIntPtr _length;

        public ulong length
        {
            get {
                return _length.ToUInt64();
            }
            set {
                _length = (UIntPtr)value;
            }
        }
    }

    public unsafe struct C4KeyValueList
    {
    }

    public unsafe struct C4Key
    {
    }
}