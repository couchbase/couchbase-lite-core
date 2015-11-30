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
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

#if NET_3_5
using CBForest.Extensions;
#endif

namespace CBForest
{
    /// <summary>
    /// An error value. These are returned by reference from API calls whose last parameter is a
    /// C4Error*. A caller can pass null if it doesn't care about the error.
    /// </summary>
    public struct C4Error
    {
        /// <summary>
        /// The domain of the error
        /// </summary>
        public C4ErrorDomain domain;

        /// <summary>
        /// The error code
        /// </summary>
        public int code;

        #pragma warning disable 1591
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
        #pragma warning restore 1591
    }

    /// <summary>
    /// Helper class for marshalling string <-> C4Slice without creating an extra copy
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
                return C4Slice.NULL;
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

        #pragma warning disable 1591
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
        #pragma warning restore 1591
    }

    /// <summary>
    /// Describes a raw document (i.e. info or _local)
    /// </summary>
    public struct C4RawDocument
    {
        /// <summary>
        /// The document key
        /// </summary>
        public C4Slice key;

        /// <summary>
        /// Meta information about the document
        /// </summary>
        public C4Slice meta;

        /// <summary>
        /// The document body
        /// </summary>
        public C4Slice body;
    }
    
    /// <summary>
    /// A revision object
    /// </summary>
    public struct C4Revision
    {
        /// <summary>
        /// The revision ID
        /// </summary>
        public C4Slice revID;

        /// <summary>
        /// Flags with information about the revision
        /// </summary>
        public C4RevisionFlags flags;

        /// <summary>
        /// The revision sequence number
        /// </summary>
        public ulong sequence;

        /// <summary>
        /// The revision body
        /// </summary>
        public C4Slice body;

        /// <summary>
        /// Gets whether or not this revision is deleted
        /// </summary>
        public bool IsDeleted
        {
            get {
                return flags.HasFlag(C4RevisionFlags.RevDeleted);
            }
        }

        /// <summary>
        /// Gets whether or not this revision is a leaf (i.e. has no children)
        /// </summary>
        public bool IsLeaf
        {
            get {
                return flags.HasFlag(C4RevisionFlags.RevLeaf);
            }
        }

        /// <summary>
        /// Gets whether or not this revision is new
        /// </summary>
        public bool IsNew
        {
            get {
                return flags.HasFlag(C4RevisionFlags.RevNew);
            }
        }

        /// <summary>
        /// Gets whether or not this revision has attachments
        /// </summary>
        public bool HasAttachments
        {
            get {
                return flags.HasFlag(C4RevisionFlags.RevHasAttachments);
            }
        }

        /// <summary>
        /// Gets whether or not this revision is active (i.e. non-deleted and current)
        /// </summary>
        public bool IsActive
        {
            get {
                return IsLeaf && !IsDeleted;
            }
        }
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

        /// <summary>
        /// The currently selected revision of this document
        /// </summary>
        public C4Revision selectedRev;

        /// <summary>
        /// Gets whether or not the document is deleted
        /// </summary>
        public bool IsDeleted
        {
            get {
                return flags.HasFlag(C4DocumentFlags.Deleted);
            }
        }

        /// <summary>
        /// Gets whether or not the document is conflicted
        /// </summary>
        public bool IsConflicted
        {
            get {
                return flags.HasFlag(C4DocumentFlags.Conflicted);
            }
        }

        /// <summary>
        /// Gets whether or not the document has attachments
        /// </summary>
        public bool HasAttachments
        {
            get {
                return flags.HasFlag(C4DocumentFlags.HasAttachments);
            }
        }

        /// <summary>
        /// Gets whether or not this document has any revisions
        /// </summary>
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
        public ulong skip;

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

        /// <summary>
        /// How many documents to skip before starting the query enumeration
        /// </summary>
        public ulong skip;   

        /// <summary>
        /// The maximum number of documents to enumerate
        /// </summary>
        public ulong limit;
        private byte _descending;
        private byte _inclusiveStart;
        private byte _inclusiveEnd;

        /// <summary>
        /// The key to start enumerating at
        /// </summary>
        public C4Key* startKey;

        /// <summary>
        /// The key to end enumerating at
        /// </summary>
        public C4Key* endKey;

        /// <summary>
        /// The document ID of the document to start enumerating at
        /// </summary>
        public C4Slice startKeyDocID;

        /// <summary>
        /// The document ID of the document to end enumerating at
        /// </summary>
        public C4Slice endKeyDocID;

        /// <summary>
        /// A list of keys to enumerate
        /// </summary>
        public C4Key** keys;
        private UIntPtr _keysCount;

        /// <summary>
        /// Gets or sets whether or not to enumerate in descending order
        /// </summary>
        public bool descending 
        { 
            get { return Convert.ToBoolean (_descending); } 
            set { _descending = Convert.ToByte(value); }
        }

        /// <summary>
        /// Gets or sets whether or not to include the first
        /// document of the enumeration in the results
        /// </summary>
        public bool inclusiveStart
        { 
            get { return Convert.ToBoolean(_inclusiveStart); }
            set { _inclusiveStart = Convert.ToByte(value); }
        }

        /// <summary>
        /// Gets or sets whether or not to include the last
        /// document of the enumeration in the results
        /// </summary>
        public bool inclusiveEnd
        { 
            get { return Convert.ToBoolean(_inclusiveEnd); }
            set { _inclusiveEnd = Convert.ToByte(value); }
        }

        /// <summary>
        /// Gets or sets wthe number of keys in the keys array
        /// </summary>
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
        /// <summary>
        /// The current key in the index
        /// </summary>
        public C4KeyReader key;   

        /// <summary>
        /// The current value in the index
        /// </summary>
        public C4Slice value;

        /// <summary>
        /// The document ID of the document this index
        /// entry came from
        /// </summary>
        public C4Slice docID;

        /// <summary>
        /// The sequence number of the document this index
        /// entry came from
        /// </summary>
        public ulong docSequence;
    }

    /// <summary>
    /// A class representing a key for encrypting a database
    /// </summary>
    public unsafe struct C4EncryptionKey
    {
        /// <summary>
        /// The default mode of encryption used by this class
        /// </summary>
        #if FAKE_C4_ENCRYPTION
        public const C4EncryptionAlgorithm ENCRYPTION_MODE = (C4EncryptionAlgorithm)(-1);
        #else
        public const C4EncryptionAlgorithm ENCRYPTION_MODE = C4EncryptionAlgorithm.AES256;
        #endif

        /// <summary>
        /// The algorithm being used by th ekey
        /// </summary>
        public C4EncryptionAlgorithm algorithm;

        /// <summary>
        /// The key data of the key (256-bit)
        /// </summary>
        public fixed byte bytes[32];

        /// <summary>
        /// Constructor
        /// </summary>
        /// <param name="algorithm">The algorithm to use when encrypting.</param>
        /// <param name="source">The key data to use.</param>
        public C4EncryptionKey(C4EncryptionAlgorithm algorithm, byte[] source)
        {
            this.algorithm = algorithm;
            fixed(byte* dest = bytes)
            fixed(byte* src = source) {
                Native.memcpy(dest, src, (UIntPtr)32U);
            }
        }

        /// <summary>
        /// Constructor
        /// </summary>
        /// <param name="source">The key data to use.</param>
        public C4EncryptionKey(byte[] source) : this(ENCRYPTION_MODE, source)
        {

        }
    }
}

