//
//  Defines.cs
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

namespace CBForest
{
    
    // A logging callback that is used to glue together managed and unmanaged
    // The callback the user receives will contain System.String instead of
    // C4Slice
    [UnmanagedFunctionPointer(CallingConvention.Cdecl, CharSet=CharSet.Ansi)]
    internal delegate void C4LogCallback(C4LogLevel level, C4Slice message);

    /// <summary>
    /// The type of encryption to use when opening a ForestDB based
    /// database
    /// </summary>
    public enum C4EncryptionAlgorithm
    {
        /// <summary>
        /// No encryption
        /// </summary>
        None = 0,

        /// <summary>
        /// AES 256-bit encryption
        /// </summary>
        AES256 = 1
    }
    
    /// <summary>
    /// Logging levels
    /// </summary>
    public enum C4LogLevel
    {
        /// <summary>
        /// DEBUG logging level (only for debug builds)
        /// </summary>
        Debug,
        /// <summary>
        /// INFO logging level
        /// </summary>
        Info,
        /// <summary>
        /// WARNING logging level
        /// </summary>
        Warning,
        /// <summary>
        /// ERROR logging level
        /// </summary>
        Error
    }
    
    /// <summary>
    /// Domain of the error code returned in a C4Error object
    /// </summary>
    public enum C4ErrorDomain : uint
    {
        /// <summary>
        /// code is an HTTP status code
        /// </summary>
        HTTP,
        /// <summary>
        /// code is an errno
        /// </summary>
        POSIX,
        /// <summary>
        /// code is a fdb_status
        /// </summary>
        ForestDB,
        /// <summary>
        /// code is C4-specific code (TBD)
        /// </summary>
        C4,
        /// <summary>
        /// Used internally for filtering on any of the above
        /// </summary>
        Any = UInt32.MaxValue
    }

    /// <summary>
    /// Error codes for the C4 Domain
    /// </summary>
    public enum C4ErrorCode
    {
        /// <summary>
        /// CBForest threw an unexpected C++ exception
        /// </summary>
        InternalException = 1,
        /// <summary>
        /// Function must be called while in a transaction
        /// </summary>
        NotInTransaction,
        /// <summary>
        /// Database can't be closed while a transaction is open
        /// </summary>
        TransactionNotClosed,
        /// <summary>
        /// Object in key is not JSON-compatible
        /// </summary>
        InvalidKey,
        /// <summary>
        /// An invalid parameter was passed to an API call
        /// </summary>
        InvalidParameter
    }

    /// <summary>
    /// Error codes for the HTTP domain
    /// </summary>
    public enum HttpErrorCode
    {
        /// <summary>
        /// HTTP Bad Request
        /// </summary>
        BadRequest = 400,
        /// <summary>
        /// HTTP Not Found
        /// </summary>
        NotFound = 404,
        /// <summary>
        /// HTTP Conflict
        /// </summary>
        Conflict = 409,
        /// <summary>
        /// HTTP Gone
        /// </summary>
        Gone = 410
    }
    
    /// <summary>
    /// Boolean options specified when opening a database or view
    /// </summary>
    [Flags]
    public enum C4DatabaseFlags : uint
    {
        /// <summary>
        /// Create the database if it does not exist
        /// </summary>
        Create = 1,
        /// <summary>
        /// Open the database as readonly
        /// </summary>
        ReadOnly = 2,
        /// <summary>
        /// Automatically compact the database when needed
        /// </summary>
        AutoCompact = 4
    }

    /// <summary>
    /// Flags describing a document.
    /// </summary>
    [Flags]
    public enum C4DocumentFlags : uint
    {
        /// <summary>
        /// The document's current revision is deleted.
        /// </summary>
        Deleted = 0x01,
        /// <summary>
        /// The document is in conflict.
        /// </summary>
        Conflicted = 0x02,
        /// <summary>
        /// The document's current revision has attachments.
        /// </summary>
        HasAttachments = 0x04,
        /// <summary>
        /// The document exists (i.e. has revisions.)
        /// </summary>
        Exists = 0x1000
    }

    /// <summary>
    /// Options for enumerating over documents. */
    /// </summary>
    [Flags]
    public enum C4EnumeratorFlags : ushort
    {
        /// <summary>
        /// If true, iteration goes by descending document IDs.
        /// </summary>
        Descending = 0x01,

        /// <summary>
        /// If false, iteration starts just _after_ startDocID.
        /// </summary>
        InclusiveStart = 0x02,

        /// <summary>
        /// If false, iteration stops just _before_ endDocID.
        /// </summary>
        InclusiveEnd = 0x04,

        /// <summary>
        /// If true, include deleted documents.
        /// </summary>
        IncludeDeleted = 0x08,

        /// <summary>
        /// If false, include _only_ documents in conflict.
        /// </summary>
        IncludeNonConflicted = 0x10,

        /// <summary>
        /// If false, document bodies will not be preloaded, just
        /// metadata (docID, revID, sequence, flags.) This is faster if you
        /// don't need to access the revision tree or revision bodies. You
        /// can still access all the data of the document, but it will
        /// trigger loading the document body from the database.
        /// </summary>
        IncludeBodies = 0x20
    }

    /// <summary>
    /// Flags that apply to a revision.
    /// </summary>
    [Flags]
    public enum C4RevisionFlags : byte
    {
        /// <summary>
        /// Is this revision a deletion/tombstone?
        /// </summary>
        RevDeleted = 0x01,
        /// <summary>
        /// Is this revision a leaf (no children?)
        /// </summary>
        RevLeaf = 0x02,
        /// <summary>
        /// Has this rev been inserted since decoding?
        /// </summary>
        RevNew = 0x04,
        /// <summary>
        /// Does this rev's body contain attachments?
        /// </summary>
        RevHasAttachments = 0x08
    }

    /// <summary>
    /// The types of tokens in a key.
    /// </summary>
    public enum C4KeyToken
    {
        /// <summary>
        /// A null value
        /// </summary>
        Null,
        /// <summary>
        /// A boolean value
        /// </summary>
        Bool,
        /// <summary>
        /// A numeric value
        /// </summary>
        Number,
        /// <summary>
        /// A text value
        /// </summary>
        String,
        /// <summary>
        /// A begin array indicator
        /// </summary>
        Array,
        /// <summary>
        /// A begin map (dictionary) indicator
        /// </summary>
        Map,
        /// <summary>
        /// The end of either an array or map
        /// </summary>
        EndSequence,
        /// <summary>
        /// A special reserved token
        /// </summary>
        Special,
        /// <summary>
        /// Represents an error
        /// </summary>
        Error = 255
    }

    /// <summary>
    /// ForestDB status codes
    /// </summary>
    public enum ForestDBStatus
    {
        /// <summary>
        /// ForestDB operation success.
        /// </summary>
        Success = 0,

        /// <summary>
        /// Invalid parameters to ForestDB APIs.
        /// </summary>
        InvalidArgs = -1,

        /// <summary>
        /// Database open operation fails.
        /// </summary>
        OpenFail = -2,

        /// <summary>
        /// Database file not found.
        /// </summary>
        NoSuchFile = -3,

        /// <summary>
        /// Database write operation fails.
        /// </summary>
        WriteFail = -4,

        /// <summary>
        /// Database read operation fails.
        /// </summary>
        ReadFail = -5,

        /// <summary>
        /// Database close operation fails.
        /// </summary>
        CloseFail = -6,

        /// <summary>
        /// Database commit operation fails.
        /// </summary>
        CommitFail = -7,

        /// <summary>
        /// Memory allocation fails.
        /// </summary>
        AllocFail = -8,

        /// <summary>
        /// A key not found in database.
        /// </summary>
        KeyNotFound = -9,

        /// <summary>
        /// Read-only access violation.
        /// </summary>
        ReadOnlyViolation = -10,

        /// <summary>
        /// Database compaction fails.
        /// </summary>
        CompactionFail = -11,

        /// <summary>
        /// Database iterator operation fails.
        /// </summary>
        IteratorFail = -12,

        /// <summary>
        /// ForestDB I/O seek failure.
        /// </summary>
        SeekFail = -13,

        /// <summary>
        /// ForestDB I/O fsync failure.
        /// </summary>
        FsyncFail = -14,

        /// <summary>
        /// ForestDB I/O checksum error.
        /// </summary>
        ChecksumError = -15,

        /// <summary>
        /// ForestDB I/O file corruption.
        /// </summary>
        FileCorruption = -16,

        /// <summary>
        /// ForestDB I/O compression error.
        /// </summary>
        CompressionFail = -17,

        /// <summary>
        /// A database instance with a given sequence number was not found.
        /// </summary>
        NoDbInstance = -18,

        /// <summary>
        /// Requested FDB operation failed as rollback is currently being executed.
        /// </summary>
        FailByRollback = -19,

        /// <summary>
        /// ForestDB config value is invalid.
        /// </summary>
        InvalidConfig = -20,

        /// <summary>
        /// Try to perform manual compaction when compaction daemon is enabled.
        /// </summary>
        ManualCompactionFail = -21,

        /// <summary>
        /// Open a file with invalid compaction mode.
        /// </summary>
        InvalidCompactionMode = -22,

        /// <summary>
        /// Operation cannot be performed as file handle has not been closed.
        /// </summary>
        FileIsBusy = -23,

        /// <summary>
        /// Database file remove operation fails.
        /// </summary>
        FileRemoveFail = -24,

        /// <summary>
        /// Database file rename operation fails.
        /// </summary>
        FileRenameFail = -25,

        /// <summary>
        /// Transaction operation fails.
        /// </summary>
        TransactionFail = -26,

        /// <summary>
        /// Requested FDB operation failed due to active transactions.
        /// </summary>
        FailByTransaction = -27,

        /// <summary>
        /// Requested FDB operation failed due to an active compaction task.
        /// </summary>
        RESULT_FAIL_BY_COMPACTION = -28,

        /// <summary>
        /// Filename is too long.
        /// </summary>
        TooLongFilename = -29,

        /// <summary>
        /// Passed ForestDB handle is Invalid.
        /// </summary>
        InvalidHandle = -30,

        /// <summary>
        /// A KV store not found in database.
        /// </summary>
        KVStoreNotFound = -31,

        /// <summary>
        /// There is an opened handle of the KV store.
        /// </summary>
        KVStoreBusy = -32,

        /// <summary>
        /// Same KV instance name already exists.
        /// </summary>
        InvalidKVInstanceName = -33,

        /// <summary>
        /// Custom compare function is assigned incorrectly.
        /// </summary>
        InvalidCmpFunction = -34,

        /// <summary>
        /// DB file can't be destroyed as the file is being compacted.
        /// Please retry in sometime.
        /// </summary>
        InUseByCompactor = -35,

        /// <summary>
        /// DB file used in this operation has not been opened
        /// </summary>
        FileNotOpen = -36,

        /// <summary>
        /// Buffer cache is too big to be configured because it is greater than
        /// the physical memory available.
        /// </summary>
        TooBigBufferCache = -37,

        /// <summary>
        /// No commit headers in a database file.
        /// </summary>
        NoDbHeaders = -38,

        /// <summary>
        /// DB handle is being used by another thread. Forestdb handles must not be
        /// shared among multiple threads.
        /// </summary>
        HandleBusy = -39,

        /// <summary>
        /// Asynchronous I/O is not supported in the current OS version.
        /// </summary>
        AIONotSupported = -40,

        /// <summary>
        /// Asynchronous I/O init fails.
        /// </summary>
        AIOInitFail = -41,

        /// <summary>
        /// Asynchronous I/O submit fails.
        /// </summary>
        AIOSubmitFail = -42,

        /// <summary>
        /// Fail to read asynchronous I/O events from the completion queue.
        /// </summary>
        AIOGetEventsFail = -43,

        /// <summary>
        /// Encryption error
        /// </summary>
        CryptoError = -44,

        /// <summary>
        /// The revision ID for a revision is invalid.  It is required
        /// to be hexidecimal.
        /// </summary>
        BadRevisionID = -1000,

        /// <summary>
        /// The revision data received for the revision data
        /// is not usable
        /// </summary>
        CorruptRevisionData = -1001,

        /// <summary>
        /// A value in an index database has become corrupt
        /// and cannot be read
        /// </summary>
        CorruptIndexData = -1002,

        /// <summary>
        /// An assertion failed (instead of calling assert() and aborting
        /// the program, an exception is thrown)
        /// </summary>
        AssertionFailed = -1003
    }
    
    /// <summary>
    /// Some predefined values for full text search languages
    /// </summary>
    public struct C4Language
    {
        /// <summary>
        /// Language code denoting "the default language"
        /// </summary>
        public static readonly string Default = null;
        
        /// <summary>
        /// Language code denoting "no language"
        /// </summary>
        public static readonly string None = String.Empty;
        
    }
}

