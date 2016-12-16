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
using System.Runtime.InteropServices;

namespace LiteCore
{
    public struct C4StorageEngine
    {
        public static readonly string SQLite = "SQLite";
    }
}

namespace LiteCore.Interop
{
    public unsafe partial struct C4EncryptionKey
    {
        private const int _Size = 32;

        public static readonly int Size = 32;
    }

    public unsafe partial struct C4DatabaseConfig : IDisposable
    {
        public static C4DatabaseConfig Clone(C4DatabaseConfig *source)
        {
            var retVal = new C4DatabaseConfig();
            retVal.flags = source->flags;
            retVal.versioning = source->versioning;
            retVal.encryptionKey = source->encryptionKey;
            retVal.storageEngine = source->storageEngine;

            return retVal;
        }

        public static C4DatabaseConfig Get(C4DatabaseConfig *source)
        {
            var retVal = new C4DatabaseConfig();
            retVal.flags = source->flags;
            retVal.versioning = source->versioning;
            retVal.encryptionKey = source->encryptionKey;
            retVal._storageEngine = source->_storageEngine; // Note: raw copy!

            return retVal;
        }

        public void Dispose()
        {
            storageEngine = null;
        }
    }

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public unsafe delegate void C4OnCompactCallback(void* context, [MarshalAs(UnmanagedType.U1)]bool compacting);
}
