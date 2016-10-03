//
// ForestDBStatus.cs
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
namespace LiteCore.Interop
{
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
}
