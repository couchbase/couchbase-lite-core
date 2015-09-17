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
using System.Runtime.InteropServices;
using System.Text;

namespace CBForest
{
    public enum C4ErrorDomain 
    {
        HTTP,
        POSIX,
        ForestDB,
        C4
    }
    
    public enum C4ErrorCode
    {
        InternalException = 1,
        NotInTransaction,
        TransactionNotClosed
    }
    
    public struct C4Error
    {
        public C4ErrorDomain domain;
        public int code;
    }
    
    internal struct C4String : IDisposable
    {
        private GCHandle _handle;
        
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
    
    public unsafe struct C4Slice
    {
        public static readonly C4Slice NULL = new C4Slice();
        
        public readonly void* buf;
        public readonly UIntPtr size;
        
        public C4Slice(void *buf, uint size)
        {
            this.buf = buf;
            this.size = new UIntPtr(size);
        }
        
        public static explicit operator string(C4Slice slice)
        {
            var bytes = (SByte*)slice.buf; 
            return new string(bytes, 0, (int)slice.size.ToUInt32(), Encoding.UTF8);
        }
        
        private bool Equals(C4Slice other)
        {
            return size == other.size && Native.memcmp(buf, other.buf, size) == 0;
        }
        
        private bool Equals(string other)
        {
            var bytes = Encoding.UTF8.GetBytes(other);
            fixed(byte* ptr = bytes) {
                return Native.memcmp(buf, ptr, size) == 0;
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
  
                hash = hash * 23 + (int)size.ToUInt32();
                var ptr = (byte*)buf;
                hash = hash * 23 + ptr[size.ToUInt32() - 1];
                
                return hash;
            }
        }
    }
    
    public struct C4RawDocument
    {
        public C4Slice key;
        public C4Slice meta;
        public C4Slice body;
    }
    
    [Flags]
    public enum C4DocumentFlags
    {
        Deleted = 0x01,
        Conflicted = 0x02,
        HasAttachments = 0x04,
        Exists = 0x1000
    }
    
    [Flags]
    public enum C4RevisionFlags
    {
        RevDeleted = 0x01,
        RevLeaf = 0x02,
        RevNew = 0x04,
        RevHasAttachments = 0x08
    }
    
    public struct C4Document
    {
        public C4DocumentFlags flags;
        public C4Slice docID;
        public C4Slice revID;
        
        public struct rev
        {
            public C4Slice revID;
            public C4RevisionFlags flags;
            public ulong sequence;
            public C4Slice body;
        }
        
        public rev selectedRev;
    }
    
    public struct C4Database
    {
    }
    
    public struct C4DocEnumerator
    {
    }
}

