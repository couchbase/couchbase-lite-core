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
using System.Threading;

namespace LiteCore
{
    public struct C4StorageEngine
    {
        public static readonly string SQLite = "SQLite";
    }
}

namespace LiteCore.Interop
{
    [Flags]
    public enum C4DatabaseFlags : uint
    {
        Create = 1,
        ReadOnly = 2,
        AutoCompact = 4,
        Bundled = 8
    }

    public enum C4DocumentVersioning : uint
    {
        RevisionTrees,
        VersionVectors
    }

    public enum C4EncryptionAlgorithm : uint
    {
        None = 0,
        AES256 = 1
    }

    public unsafe struct C4EncryptionKey
    {
        private const int _Size = 32;

        public static readonly int Size = 32;

        public C4EncryptionAlgorithm algorithm;
        public fixed byte bytes[_Size];
    }

    public unsafe struct C4DatabaseConfig : IDisposable
    {
        public C4DatabaseFlags flags;
        private IntPtr _storageEngine;
        public C4DocumentVersioning versioning;
        public C4EncryptionKey encryptionKey;

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
            retVal._storageEngine = source->_storageEngine;

            return retVal;
        }

        public string storageEngine
        {
            get {
                return Marshal.PtrToStringAnsi(_storageEngine);
            }
            set {
                var old = Interlocked.Exchange(ref _storageEngine, Marshal.StringToHGlobalAnsi(value));
                Marshal.FreeHGlobal(old);
            }
        }

        public void Dispose()
        {
            storageEngine = null;
        }
    }

    public struct C4Database
    {
        
    }

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public unsafe delegate void C4OnCompactCallback(void* context, [MarshalAs(UnmanagedType.U1)]bool compacting);

    public struct C4RawDocument
    {
        public C4Slice key;
        public C4Slice meta;
        public C4Slice body;
    }
}
