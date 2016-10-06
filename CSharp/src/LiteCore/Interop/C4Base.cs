//
// Base.cs
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

using LiteCore.Util;

namespace LiteCore.Interop
{
    public enum C4ErrorDomain : uint
    {
        LiteCore = 1,
        POSIX,
        ForestDB,
        SQLite,
        Any = UInt32.MaxValue
    }

    public enum LiteCoreError {
        AssertionFailed = 1,    // Internal assertion failure
        Unimplemented,          // Oops, an unimplemented API call
        NoSequences,            // This KeyStore does not support sequences
        UnsupportedEncryption,  // Unsupported encryption algorithm
        NoTransaction,          // Function must be called within a transaction
        BadRevisionID,          // Invalid revision ID syntax
        BadVersionVector,       // Invalid version vector syntax
        CorruptRevisionData,    // Revision contains corrupted/unreadable data
        CorruptIndexData,       // Index contains corrupted/unreadable data
        TokenizerError, /*10*/  // can't create text tokenizer for FTS
        NotOpen,                // Database/KeyStore/index is not open
        NotFound,               // Document not found
        Deleted,                // Document has been deleted
        Conflict,               // Document update conflict
        InvalidParameter,       // Invalid function parameter or struct value
        DatabaseError,          // Lower-level database error (ForestDB or SQLite)
        UnexpectedError,        // Internal unexpected C++ exception
        CantOpenFile,           // Database file can't be opened; may not exist
        IOError,                // File I/O error
        CommitFailed, /*20*/    // Transaction commit failed
        MemoryError,            // Memory allocation failed (out of memory?)
        NotWriteable,           // File is not writeable
        CorruptData,            // Data is corrupted
        Busy,                   // Database is busy/locked
        NotInTransaction,       // Function cannot be called while in a transaction
        TransactionNotClosed,   // Database can't be closed while a transaction is open
        IndexBusy,              // View can't be closed while index is enumerating
        Unsupported,            // Operation not supported in this database
        NotADatabaseFile,       // File is not a database, or encryption key is wrong
        WrongFormat, /*30*/     // Database exists but not in the format/storage requested
    };

    public struct C4Error
    {
        C4ErrorDomain _domain;
        int _code;

        public C4ErrorDomain Domain
        {
            get {
                return _domain;
            }
        }

        public int Code
        {
            get {
                return _code;
            }
        }

        public C4Error(C4ErrorDomain domain, int code)
        {
            _code = code;
            _domain = domain;
        }

        public C4Error(ForestDBStatus code) : this(C4ErrorDomain.ForestDB, (int)code)
        {
            
        }

        public C4Error(SQLiteStatus code) : this(C4ErrorDomain.SQLite, (int)code)
        {
            
        }

        public C4Error(LiteCoreError code) : this(C4ErrorDomain.LiteCore, (int)code)
        {
            
        }
    }

    public unsafe struct C4Slice : IEnumerable<byte>
    {
        public static readonly C4Slice Null = new C4Slice(null, 0);

        public void* buf;
        private UIntPtr _size;

        public ulong size
        {
            get {
                return _size.ToUInt64();
            }
            set {
                _size = (UIntPtr)value;
            }
        }

        public C4Slice(void* buf, ulong size)
        {
            this.buf = buf;
            this._size = new UIntPtr(size);
        }

        public static C4Slice Constant(string input)
        {
            return (C4Slice)FLSlice.Constant(input);
        }

        private bool Equals(C4Slice other)
        {
            return Native.c4SliceEqual(this, other);
        }

        private bool Equals(string other)
        {
            var c4str = new C4String(other);
            return Equals(c4str.AsC4Slice());
        }

        public string CreateString()
        {
            if(buf == null) {
                return null;
            }

            var bytes = ToArrayFast();
            return Encoding.UTF8.GetString(bytes, 0, bytes.Length);
        }

        public byte[] ToArrayFast()
        {
            if(buf == null) {
                return null;
            }

            var tmp = new IntPtr(buf);
            var bytes = new byte[size];
            Marshal.Copy(tmp, bytes, 0, bytes.Length);
            return bytes;
        }

        public static explicit operator C4Slice(FLSlice input)
        {
            return new C4Slice(input.buf, input.size);
        }

        public static explicit operator FLSlice(C4Slice input)
        {
            return new FLSlice(input.buf, input.size);
        }

        #pragma warning disable 1591

        public override string ToString()
        {
            return String.Format("C4Slice[\"{0}\"]", CreateString());
        }

        public override bool Equals(object obj)
        {
            if(obj is C4Slice) {
                return Equals((C4Slice)obj);
            }

            var str = obj as string;
            return str != null && Equals(str);
        }

        public override int GetHashCode()
        {
            unchecked {
                int hash = 17;

                hash = hash * 23 + (int)size;
                var ptr = (byte*)buf;
                if(ptr != null) {
                    hash = hash * 23 + ptr[size - 1];
                }

                return hash;
            }
        }

        public IEnumerator<byte> GetEnumerator()
        {
            return new C4SliceEnumerator(buf, (int)size);
        }

        System.Collections.IEnumerator System.Collections.IEnumerable.GetEnumerator()
        {
            return GetEnumerator();
        }
#pragma warning restore 1591
    }

    public unsafe struct C4SliceResult : IDisposable
    {
        public void* buf;
        private UIntPtr _size;

        public ulong size
        {
            get {
                return _size.ToUInt64();
            }
            set {
                _size = (UIntPtr)value;
            }
        }

        public static implicit operator C4Slice(C4SliceResult input)
        {
            return new C4Slice(input.buf, input.size);
        }

        public void Dispose()
        {
            Native.c4slice_free(this);
        }
    }

    public enum C4LogLevel : byte
    {
        Debug,
        Info,
        Warning,
        Error
    }

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void C4LogCallback(C4LogLevel level, C4Slice message);

    public unsafe static partial class Native
    {
        private static readonly byte[] _ErrorBuf = new byte[50];

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static extern bool c4SliceEqual(C4Slice a, C4Slice b);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void c4slice_free(C4Slice slice);

        public static string c4error_getMessage(C4Error err)
        {
            using(var retVal = NativeRaw.c4error_getMessage(err)) {
                return ((C4Slice)retVal).CreateString();
            }
        }

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void c4log_register(C4LogLevel level, C4LogCallback callback);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void c4log_setLevel(C4LogLevel level);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void c4log_warnOnErrors([MarshalAs(UnmanagedType.U1)]bool warn);

        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int c4_getObjectCount();
    }

    public unsafe static partial class NativeRaw
    {
        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern C4SliceResult c4error_getMessage(C4Error error);
    }

}
