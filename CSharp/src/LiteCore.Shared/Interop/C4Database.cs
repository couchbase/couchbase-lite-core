//
//  C4Database.cs
//
//  Author:
//   Jim Borden  <jim.borden@couchbase.com>
//
//  Copyright (c) 2017 Couchbase, Inc All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License");
//  you may not use this file except in compliance with the License.
//  You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.
//
using System;
using System.Runtime.InteropServices;

namespace LiteCore
{
#if LITECORE_PACKAGED
    internal
#else
    public
#endif
         struct C4StorageEngine
    {
        public static readonly string SQLite = "SQLite";
    }
}

namespace LiteCore.Interop
{
#if LITECORE_PACKAGED
    internal
#else
    public
#endif
        partial struct C4EncryptionKey
    {
        // ReSharper disable InconsistentNaming
        private const int _Size = 32;
        // ReSharper restore InconsistentNaming

        public static readonly int Size = _Size;
    }

#if LITECORE_PACKAGED
    internal
#else
    public
#endif
         unsafe partial struct C4UUID
    {
        // ReSharper disable InconsistentNaming
        private const int _Size = 32;
        // ReSharper restore InconsistentNaming

        public static readonly int Size = 32;

        public override int GetHashCode()
        {
                        int hash = 17;
            unchecked {
                fixed(byte* b = bytes) {
                    for(int i = 0; i < _Size; i++) {
                        hash = hash * 23 + b[i];
                    }
                }
            }

            return hash;
        }

        public override bool Equals(object obj)
        {
                        if(!(obj is C4UUID)) {
                return false;
            }

            var other = (C4UUID)obj;
            fixed(byte* b = bytes) {
                for(var i = 0; i < _Size; i++) {
                    if(b[i] != other.bytes[i]) {
                        return false;
                    }
                }
            }

            return true;
        }
    }

#if LITECORE_PACKAGED
    internal
#else
    public
#endif
         unsafe partial struct C4DatabaseConfig : IDisposable
    {
        public static C4DatabaseConfig Clone(C4DatabaseConfig *source)
        {
            var retVal = new C4DatabaseConfig {
                flags = source->flags,
                versioning = source->versioning,
                encryptionKey = source->encryptionKey,
                storageEngine = source->storageEngine
            };

            return retVal;
        }

        public static C4DatabaseConfig Get(C4DatabaseConfig *source)
        {
            var retVal = new C4DatabaseConfig {
                flags = source->flags,
                versioning = source->versioning,
                encryptionKey = source->encryptionKey,
                _storageEngine = source->_storageEngine // Note: raw copy!
            };

            return retVal;
        }

        public void Dispose()
        {
            storageEngine = null;
        }
    }

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
#if LITECORE_PACKAGED
    internal
#else
    public
#endif
         unsafe delegate void C4OnCompactCallback(void* context, [MarshalAs(UnmanagedType.U1)]bool compacting);
}
