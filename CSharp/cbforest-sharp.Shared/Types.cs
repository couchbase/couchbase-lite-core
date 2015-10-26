//
//  Types.cs
//
//  Author:
//  	Jim Borden  <jim.borden@couchbase.com>
//
//  Copyright (c) 2015 Couchbase, Inc All rights reserved.
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
#define FAKE_C4_ENCRYPTION
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

namespace CBForest
{
    /// <summary>
    /// An error value. These are returned by reference from API calls whose last parameter is a
    /// C4Error*. A caller can pass null if it doesn't care about the error.
    /// </summary>
    public struct C4Error
    {
        public C4ErrorDomain domain;
        public int code;

        public override string ToString()
        {
            if (domain == C4ErrorDomain.C4) {
                return String.Format("[C4Error {0}]", (C4ErrorCode)code);
            }

            if (domain == C4ErrorDomain.ForestDB) {
                return String.Format("[C4Error {0}]", (ForestDBStatus)code);
            }

            return String.Format("[C4Error {0},{1}]", domain, code);
        }
    }

    /// <summary>
    /// Helper class for marshalling string <-> C4Slice without creating an extra copy
    /// of the bytes.  Not for storage or long-term use
    /// </summary>
    public struct C4String : IDisposable
    {
        private GCHandle _handle; // Stores the UTF-8 bytes in a pinned location
        
        public C4String(string s)
        {
            _handle = new GCHandle();
            if(s != null) {
                var bytes = Encoding.UTF8.GetBytes(s);
                _handle = GCHandle.Alloc(bytes, GCHandleType.Pinned);  
            }
        }
        
        public void Dispose()
        {
            if(_handle.IsAllocated) {
                _handle.Free();   
            }
        }
        
        public unsafe C4Slice AsC4Slice()
        {
            if(!_handle.IsAllocated || _handle.Target == null) {
                return C4Slice.NULL;
            }

            var bytes = _handle.Target as byte[];
            return new C4Slice(_handle.AddrOfPinnedObject().ToPointer(), (uint)bytes.Length);
        }
    }

    internal unsafe struct C4SliceEnumerator : IEnumerator<byte>
    {
        private readonly byte* _start;
        private byte* _current;
        private readonly int _length;

        public C4SliceEnumerator(void *buf, int length)
        {
            _start = (byte*)buf;
            _current = _start - 1;
            _length = length;
        }

        public bool MoveNext()
        {
            if ((_current - _start) >= _length - 1) {
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

    /// <summary>
    /// A slice is simply a pointer to a range of bytes, usually interpreted as a UTF-8 string.
    ///  A "null slice" has chars==NULL and length==0.
    /// A slice with length==0 is not necessarily null; if chars!=NULL it's an empty string.
    /// A slice as a function parameter is temporary and read-only: the function will not alter or free
    /// the bytes, and the pointer won't be accessed after the function returns.
    /// A slice _returned from_ a function points to newly-allocated memory and must be freed by the
    /// caller, with c4slice_free().
    /// </summary>
    public unsafe struct C4Slice : IEnumerable<byte>
    {
        /// <summary>
        /// A convenient constant denoting a null slice.  Note that as a struct
        /// it is not required to be new'd and will have its contents zero'd by
        /// the runtime
        /// </summary>
        public static readonly C4Slice NULL = default(C4Slice);
        
        /// <summary>
        /// The data being held by this instance
        /// </summary>
        public readonly void* buf;

        private UIntPtr _size;

        /// <summary>
        /// The size of the data being held by this instance
        /// </summary>
        public uint size
        {
            get { return _size.ToUInt32(); }
            set { _size = (UIntPtr)value; }
        }
        
        /// <summary>
        /// Constructor
        /// </summary>
        /// <param name="buf">The data to store</param>
        /// <param name="size">The size of the data to store</param>
        public C4Slice(void *buf, uint size)
        {
            this.buf = buf;
            _size = (UIntPtr)size;
        }
        
        /// <summary>
        /// Explicit string cast.  Creates a new string from the stored data
        /// with UTF-8 encoding.
        /// </summary>
        /// <param name="slice">The slice to convert to a string</param>
        public static explicit operator string(C4Slice slice)
        {
            if (slice.buf == null) {
                return null;
            }

            var bytes = (sbyte*)slice.buf; 
            return new string(bytes, 0, (int)slice.size, Encoding.UTF8);
        }
        
        private bool Equals(C4Slice other)
        {
            return size == other.size && Native.memcmp(buf, other.buf, _size) == 0;
        }
        
        private bool Equals(string other)
        {
            var bytes = Encoding.UTF8.GetBytes(other);
            fixed(byte* ptr = bytes) {
                return Native.memcmp(buf, ptr, _size) == 0;
            }
        }
        
        public override string ToString()
        {
            return String.Format("C4Slice[\"{0}\"]", (string)this);
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
            unchecked 
            {
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

    }

    /// <summary>
    /// Describes a raw document (i.e. info or _local)
    /// </summary>
    public struct C4RawDocument
    {
        public C4Slice key;
        public C4Slice meta;
        public C4Slice body;
    }

    /// <summary>
    /// Describes a version-controlled document.
    /// </summary>
    public struct C4Document
    {
        /// <summary>
        /// Document flags
        /// </summary>
        public C4DocumentFlags flags;

        /// <summary>
        /// Document ID
        /// </summary>
        public C4Slice docID;

        /// <summary>
        /// Revision ID of the current revision
        /// </summary>
        public C4Slice revID;

        /// <summary>
        /// Sequence at which the doc was last updated
        /// </summary>
        public ulong sequence;
        
        public struct rev
        {
            public C4Slice revID;
            public C4RevisionFlags flags;
            public ulong sequence;
            public C4Slice body;

            public bool IsDeleted
            {
                get {
                    return flags.HasFlag(C4RevisionFlags.RevDeleted);
                }
            }

            public bool IsLeaf
            {
                get {
                    return flags.HasFlag(C4RevisionFlags.RevLeaf);
                }
            }

            public bool IsNew
            {
                get {
                    return flags.HasFlag(C4RevisionFlags.RevNew);
                }
            }

            public bool HasAttachments
            {
                get {
                    return flags.HasFlag(C4RevisionFlags.RevHasAttachments);
                }
            }

            public bool IsActive
            {
                get {
                    return IsLeaf && !IsDeleted;
                }
            }
        }
        
        public rev selectedRev;

        public bool IsDeleted
        {
            get {
                return flags.HasFlag(C4DocumentFlags.Deleted);
            }
        }

        public bool IsConflicted
        {
            get {
                return flags.HasFlag(C4DocumentFlags.Conflicted);
            }
        }

        public bool HasAttachments
        {
            get {
                return flags.HasFlag(C4DocumentFlags.HasAttachments);
            }
        }

        public bool Exists
        {
            get {
                return flags.HasFlag(C4DocumentFlags.Exists);
            }
        }
    }

    /// <summary>
    /// Options for enumerating over all documents.
    /// </summary>
    public struct C4EnumeratorOptions
    {
        /// <summary>
        /// Default enumeration options.
        /// </summary>
        public static readonly C4EnumeratorOptions DEFAULT =  new C4EnumeratorOptions { 
            flags = C4EnumeratorFlags.InclusiveStart |
                C4EnumeratorFlags.InclusiveEnd |
                C4EnumeratorFlags.IncludeNonConflicted |
                C4EnumeratorFlags.IncludeBodies 
            };

        /// <summary>
        /// The number of initial results to skip.
        /// </summary>
        public uint skip;

        /// <summary>
        /// Option flags
        /// </summary>
        public C4EnumeratorFlags flags;
    }

    /// <summary>
    /// An opaque struct pointing to the raw data of an encoded key. The functions that operate
    /// on this allow it to be parsed by reading items one at a time (similar to SAX parsing.)
    /// </summary>
    public unsafe struct C4KeyReader
    {
        /// <summary>
        /// The raw data in the key
        /// </summary>
        public readonly void* bytes;

        private readonly UIntPtr _length;

        /// <summary>
        /// The length of the raw data in the key
        /// </summary>
        public uint length
        {
            get { return _length.ToUInt32(); }
        }
    }

    /// <summary>
    /// Opaque handle to an opened database.
    /// </summary>
    public struct C4Database
    {
    }

    /// <summary>
    /// Opaque handle to a document enumerator.
    /// </summary>
    public struct C4DocEnumerator
    {
    }

    /// <summary>
    /// An opaque value used as a key or value in a view index. JSON-compatible.
    /// </summary>
    public struct C4Key
    {   
    }

    /// <summary>
    /// Opaque handle to an opened view.
    /// </summary>
    public struct C4View
    {
    }

    /// <summary>
    /// Opaque reference to an indexing task.
    /// </summary>
    public struct C4Indexer
    {
    }

    /// <summary>
    /// Options for view queries.
    /// </summary>
    public unsafe struct C4QueryOptions
    {
        /// <summary>
        /// Default query options.
        /// </summary>
        public static readonly C4QueryOptions DEFAULT = 
            new C4QueryOptions { limit = UInt32.MaxValue, inclusiveStart = true, inclusiveEnd = true };
        
        public ulong skip;   
        public ulong limit;
        private byte _descending;
        private byte _inclusiveStart;
        private byte _inclusiveEnd;
        
        public C4Key* startKey;
        public C4Key* endKey;
        public C4Slice startKeyDocID;
        public C4Slice endKeyDocID;
        public C4Key** keys;
        private UIntPtr _keysCount;

        public bool descending 
        { 
            get { return Convert.ToBoolean (_descending); } 
            set { _descending = Convert.ToByte(value); }
        }
        
        public bool inclusiveStart
        { 
            get { return Convert.ToBoolean(_inclusiveStart); }
            set { _inclusiveStart = Convert.ToByte(value); }
        }
        
        public bool inclusiveEnd
        { 
            get { return Convert.ToBoolean(_inclusiveEnd); }
            set { _inclusiveEnd = Convert.ToByte(value); }
        }

        public uint keysCount
        {
            get { return _keysCount.ToUInt32(); }
            set { _keysCount = (UIntPtr)value; }
        }
    }

    /// <summary>
    /// A view query result enumerator. Created by c4view_query.
    /// The fields of the object are invalidated by the next call to c4queryenum_next or
    /// c4queryenum_free.
    /// </summary>
    public struct C4QueryEnumerator
    {
        public C4KeyReader key;   
        public C4Slice value;
        public C4Slice docID;
        public ulong docSequence;
    }


    public unsafe struct C4EncryptionKey
    {
        #if FAKE_C4_ENCRYPTION
        public const C4EncryptionType ENCRYPTION_MODE = (C4EncryptionType)(-1);
        #else
        public const C4EncryptionType ENCRYPTION_MODE = C4EncryptionType.AES256;
        #endif

        public C4EncryptionType algorithm;

        public fixed byte bytes[32];

        public C4EncryptionKey(C4EncryptionType algorithm, byte[] source)
        {
            this.algorithm = algorithm;
            fixed(byte* dest = bytes)
            fixed(byte* src = source) {
                Native.memcpy(dest, src, (UIntPtr)32U);
            }
        }

        public C4EncryptionKey(byte[] source) : this(ENCRYPTION_MODE, source)
        {

        }
    }
}

