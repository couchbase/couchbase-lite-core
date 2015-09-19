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

namespace CBForest
{
    /// <summary>
    /// Domain of the error code returned in a C4Error object
    /// </summary>
    public enum C4ErrorDomain 
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
        C4
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
        InvalidKey
    }

    /// <summary>
    /// Flags describing a document.
    /// </summary>
    [Flags]
    public enum C4DocumentFlags
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
    /// Flags that apply to a revision.
    /// </summary>
    [Flags]
    public enum C4RevisionFlags
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
    
    public enum fdb_status
    {
        /// <summary>
        /// ForestDB operation success.
        /// </summary>
        RESULT_SUCCESS = 0,

        /// <summary>
        /// Invalid parameters to ForestDB APIs.
        /// </summary>
        RESULT_INVALID_ARGS = -1,

        /// <summary>
        /// Database open operation fails.
        /// </summary>
        RESULT_OPEN_FAIL = -2,

        /// <summary>
        /// Database file not found.
        /// </summary>
        RESULT_NO_SUCH_FILE = -3,

        /// <summary>
        /// Database write operation fails.
        /// </summary>
        RESULT_WRITE_FAIL = -4,

        /// <summary>
        /// Database read operation fails.
        /// </summary>
        RESULT_READ_FAIL = -5,

        /// <summary>
        /// Database close operation fails.
        /// </summary>
        RESULT_CLOSE_FAIL = -6,

        /// <summary>
        /// Database commit operation fails.
        /// </summary>
        RESULT_COMMIT_FAIL = -7,

        /// <summary>
        /// Memory allocation fails.
        /// </summary>
        RESULT_ALLOC_FAIL = -8,

        /// <summary>
        /// A key not found in database.
        /// </summary>
        RESULT_KEY_NOT_FOUND = -9,

        /// <summary>
        /// Read-only access violation.
        /// </summary>
        RESULT_RONLY_VIOLATION = -10,

        /// <summary>
        /// Database compaction fails.
        /// </summary>
        RESULT_COMPACTION_FAIL = -11,

        /// <summary>
        /// Database iterator operation fails.
        /// </summary>
        RESULT_ITERATOR_FAIL = -12,

        /// <summary>
        /// ForestDB I/O seek failure.
        /// </summary>
        RESULT_SEEK_FAIL = -13,

        /// <summary>
        /// ForestDB I/O fsync failure.
        /// </summary>
        RESULT_FSYNC_FAIL = -14,

        /// <summary>
        /// ForestDB I/O checksum error.
        /// </summary>
        RESULT_CHECKSUM_ERROR = -15,

        /// <summary>
        /// ForestDB I/O file corruption.
        /// </summary>
        RESULT_FILE_CORRUPTION = -16,

        /// <summary>
        /// ForestDB I/O compression error.
        /// </summary>
        RESULT_COMPRESSION_FAIL = -17,

        /// <summary>
        /// A database instance with a given sequence number was not found.
        /// </summary>
        RESULT_NO_DB_INSTANCE = -18,

        /// <summary>
        /// Requested FDB operation failed as rollback is currently being executed.
        /// </summary>
        RESULT_FAIL_BY_ROLLBACK = -19,

        /// <summary>
        /// ForestDB config value is invalid.
        /// </summary>
        RESULT_INVALID_CONFIG = -20,

        /// <summary>
        /// Try to perform manual compaction when compaction daemon is enabled.
        /// </summary>
        RESULT_MANUAL_COMPACTION_FAIL = -21,

        /// <summary>
        /// Open a file with invalid compaction mode.
        /// </summary>
        RESULT_INVALID_COMPACTION_MODE = -22,

        /// <summary>
        /// Operation cannot be performed as file handle has not been closed.
        /// </summary>
        RESULT_FILE_IS_BUSY = -23,

        /// <summary>
        /// Database file remove operation fails.
        /// </summary>
        RESULT_FILE_REMOTE_FAIL = -24,

        /// <summary>
        /// Database file rename operation fails.
        /// </summary>
        RESULT_FILE_RENAME_FAIL = -25,

        /// <summary>
        /// Transaction operation fails.
        /// </summary>
        RESULT_TRANSACTION_FAIL = -26,

        /// <summary>
        /// Requested FDB operation failed due to active transactions.
        /// </summary>
        RESULT_FAIL_BY_TRANSACTION = -27,

        /// <summary>
        /// Requested FDB operation failed due to an active compaction task.
        /// </summary>
        RESULT_FAIL_BY_COMPACTION = -28,

        /// <summary>
        /// Filename is too long.
        /// </summary>
        RESULT_TOO_LONG_FILENAME = -29,

        /// <summary>
        /// Passed ForestDB handle is Invalid.
        /// </summary>
        RESULT_INVALID_HANDLE = -30,

        /// <summary>
        /// A KV store not found in database.
        /// </summary>
        RESULT_KV_STORE_NOT_FOUND = -31,

        /// <summary>
        /// There is an opened handle of the KV store.
        /// </summary>
        RESULT_KV_STORE_BUSY = -32,

        /// <summary>
        /// Same KV instance name already exists.
        /// </summary>
        RESULT_INVALID_KV_INSTANCE_NAME = -33,

        /// <summary>
        /// Custom compare function is assigned incorrectly.
        /// </summary>
        RESULT_INVALID_CMP_FUNCTION = -34,

        /// <summary>
        /// DB file can't be destroyed as the file is being compacted.
        /// Please retry in sometime.
        /// </summary>
        RESULT_IN_USE_BY_COMPACTOR = -35,

        /// <summary>
        /// DB file used in this operation has not been opened
        /// </summary>
        RESULT_FILE_NOT_OPEN = -36,

        /// <summary>
        /// Buffer cache is too big to be configured because it is greater than
        /// the physical memory available.
        /// </summary>
        RESULT_TOO_BIG_BUFFER_CACHE = -37,

        /// <summary>
        /// No commit headers in a database file.
        /// </summary>
        RESULT_NO_DB_HEADERS = -38,

        /// <summary>
        /// DB handle is being used by another thread. Forestdb handles must not be
        /// shared among multiple threads.
        /// </summary>
        RESULT_HANDLE_BUSY = -39,

        /// <summary>
        /// Asynchronous I/O is not supported in the current OS version.
        /// </summary>
        RESULT_AIO_NOT_SUPPORTED = -40,

        /// <summary>
        /// Asynchronous I/O init fails.
        /// </summary>
        RESULT_AIO_INIT_FAIL = -41,

        /// <summary>
        /// Asynchronous I/O submit fails.
        /// </summary>
        RESULT_AIO_SUBMIT_FAIL = -42,

        /// <summary>
        /// Fail to read asynchronous I/O events from the completion queue.
        /// </summary>
        RESULT_AIO_GETEVENTS_FAIL = -43
    }
}

