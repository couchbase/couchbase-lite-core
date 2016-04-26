//  Copyright © 2015 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

package com.couchbase.cbforest;

/**
 * Created by hideki on 9/30/15.
 */
public interface Constants {

    // fdb_errors.h
    interface FDBErrors {
        /**
         * ForestDB operation success.
         */
        int FDB_RESULT_SUCCESS = 0;
        /**
         * Invalid parameters to ForestDB APIs.
         */
        int FDB_RESULT_INVALID_ARGS = -1;
        /**
         * Database open operation fails.
         */
        int FDB_RESULT_OPEN_FAIL = -2;
        /**
         * Database file not found.
         */
        int FDB_RESULT_NO_SUCH_FILE = -3;
        /**
         * Database write operation fails.
         */
        int FDB_RESULT_WRITE_FAIL = -4;
        /**
         * Database read operation fails.
         */
        int FDB_RESULT_READ_FAIL = -5;
        /**
         * Database close operation fails.
         */
        int FDB_RESULT_CLOSE_FAIL = -6;
        /**
         * Database commit operation fails.
         */
        int FDB_RESULT_COMMIT_FAIL = -7;
        /**
         * Memory allocation fails.
         */
        int FDB_RESULT_ALLOC_FAIL = -8;
        /**
         * A key not found in database.
         */
        int FDB_RESULT_KEY_NOT_FOUND = -9;
        /**
         * Read-only access violation.
         */
        int FDB_RESULT_RONLY_VIOLATION = -10;
        /**
         * Database compaction fails.
         */
        int FDB_RESULT_COMPACTION_FAIL = -11;
        /**
         * Database iterator operation fails.
         */
        int FDB_RESULT_ITERATOR_FAIL = -12;
        /**
         * ForestDB I/O seek failure.
         */
        int FDB_RESULT_SEEK_FAIL = -13;
        /**
         * ForestDB I/O fsync failure.
         */
        int FDB_RESULT_FSYNC_FAIL = -14;
        /**
         * ForestDB I/O checksum error.
         */
        int FDB_RESULT_CHECKSUM_ERROR = -15;
        /**
         * ForestDB I/O file corruption.
         */
        int FDB_RESULT_FILE_CORRUPTION = -16;
        /**
         * ForestDB I/O compression error.
         */
        int FDB_RESULT_COMPRESSION_FAIL = -17;
        /**
         * A database instance with a given sequence number was not found.
         */
        int FDB_RESULT_NO_DB_INSTANCE = -18;
        /**
         * Requested FDB operation failed as rollback is currently being executed.
         */
        int FDB_RESULT_FAIL_BY_ROLLBACK = -19;
        /**
         * ForestDB config value is invalid.
         */
        int FDB_RESULT_INVALID_CONFIG = -20;
        /**
         * Try to perform manual compaction when compaction daemon is enabled.
         */
        int FDB_RESULT_MANUAL_COMPACTION_FAIL = -21;
        /**
         * Open a file with invalid compaction mode.
         */
        int FDB_RESULT_INVALID_COMPACTION_MODE = -22;
        /**
         * Operation cannot be performed as file handle has not been closed.
         */
        int FDB_RESULT_FILE_IS_BUSY = -23;
        /**
         * Database file remove operation fails.
         */
        int FDB_RESULT_FILE_REMOVE_FAIL = -24;
        /**
         * Database file rename operation fails.
         */
        int FDB_RESULT_FILE_RENAME_FAIL = -25;
        /**
         * Transaction operation fails.
         */
        int FDB_RESULT_TRANSACTION_FAIL = -26;
        /**
         * Requested FDB operation failed due to active transactions.
         */
        int FDB_RESULT_FAIL_BY_TRANSACTION = -27;
        /**
         * Requested FDB operation failed due to an active compaction task.
         */
        int FDB_RESULT_FAIL_BY_COMPACTION = -28;
        /**
         * Filename is too long.
         */
        int FDB_RESULT_TOO_LONG_FILENAME = -29;
        /**
         * Passed ForestDB handle is Invalid.
         */
        int FDB_RESULT_INVALID_HANDLE = -30;
        /**
         * A KV store not found in database.
         */
        int FDB_RESULT_KV_STORE_NOT_FOUND = -31;
        /**
         * There is an opened handle of the KV store.
         */
        int FDB_RESULT_KV_STORE_BUSY = -32;
        /**
         * Same KV instance name already exists.
         */
        int FDB_RESULT_INVALID_KV_INSTANCE_NAME = -33;
        /**
         * Custom compare function is assigned incorrectly.
         */
        int FDB_RESULT_INVALID_CMP_FUNCTION = -34;
        /**
         * DB file can't be destroyed as the file is being compacted.
         * Please retry in sometime.
         */
        int FDB_RESULT_IN_USE_BY_COMPACTOR = -35;
        /**
         * DB file used in this operation has not been opened
         */
        int FDB_RESULT_FILE_NOT_OPEN = -36;
        /**
         * Buffer cache is too big to be configured because it is greater than
         * the physical memory available.
         */
        int FDB_RESULT_TOO_BIG_BUFFER_CACHE = -37;
        /**
         * No commit headers in a database file.
         */
        int FDB_RESULT_NO_DB_HEADERS = -38;
        /**
         * DB handle is being used by another thread. Forestdb handles must not be
         * shared among multiple threads.
         */
        int FDB_RESULT_HANDLE_BUSY = -39;
        /**
         * Asynchronous I/O is not supported in the current OS version.
         */
        int FDB_RESULT_AIO_NOT_SUPPORTED = -40;
        /**
         * Asynchronous I/O init fails.
         */
        int FDB_RESULT_AIO_INIT_FAIL = -41;
        /**
         * Asynchronous I/O submit fails.
         */
        int FDB_RESULT_AIO_SUBMIT_FAIL = -42;
        /**
         * Fail to read asynchronous I/O events from the completion queue.
         */
        int FDB_RESULT_AIO_GETEVENTS_FAIL = -43;
        /**
         * Error encrypting or decrypting data, or unsupported encryption algorithm.
         */
        int FDB_RESULT_CRYPTO_ERROR = -44;
    }

    //////// DOCUMENTS:

    // Flags describing a document.
    // Note: Superset of VersionedDocument::Flags
    interface C4DocumentFlags {
        int kDeleted = 0x01;        // The document's current revision is deleted.
        int kConflicted = 0x02;     // The document is in conflict.
        int kHasAttachments = 0x04; // One or more revisions have attachments.
        int kExists = 0x1000;       // The document exists (i.e. has revisions.)
    }

    //  Flags that apply to a revision.
    // Note: Same as Revision::Flags
    interface C4RevisionFlags {
        int kRevDeleted = 0x01;        // Is this revision a deletion/tombstone?
        int kRevLeaf = 0x02;           // Is this revision a leaf (no children?)
        int kRevNew = 0x04;            // Has this rev been inserted since decoding?
        int kRevHasAttachments = 0x08; // Does this rev's body contain attachments?
    }

    // Flags for document iteration
    interface IteratorFlags {
        int kDescending             = 0x01;
        int kInclusiveStart         = 0x02;
        int kInclusiveEnd           = 0x04;
        int kIncludeDeleted         = 0x08;
        int kIncludeNonConflicted   = 0x10;
        int kIncludeBodies          = 0x20;

        int kDefault = kInclusiveStart | kInclusiveEnd | kIncludeNonConflicted | kIncludeBodies;
    }

    interface C4ErrorDomain {
        int HTTPDomain = 0;         // code is an HTTP status code
        int POSIXDomain = 1;        // code is an errno
        int ForestDBDomain = 2;     // code is a fdb_status
        int C4Domain = 3;           // code is C4-specific code (TBD)
    }

    // Extra status codes not defined by fdb_errors.h
    interface CBForestError {
        int BadRevisionID = -1000;
        int CorruptRevisionData = -1001;
        int CorruptIndexData = -1002;
        int AssertionFailed = -1003;
    }

    // C4Domain error codes:
    interface C4DomainErrorCode {
        int kC4ErrorInternalException = 1;    // CBForest threw an unexpected C++ exception
        int kC4ErrorNotInTransaction = 2;     // Function must be called while in a transaction
        int kC4ErrorTransactionNotClosed = 3; // Database can't be closed while a transaction is open

        // These come from CBForest (error.hh)
        int kC4ErrorBadRevisionID = -1000;
        int kC4ErrorCorruptRevisionData = -1001;
        int kC4ErrorCorruptIndexData = -1002;
        int kC4ErrorAssertionFailed = -1003;
        int kC4ErrorTokenizerError = -1004;     // can't create FTS tokenizer
    }

    // The types of tokens in a key.
    interface C4KeyToken {
        int kC4Null = 0;
        int kC4Bool = 1;
        int kC4Number = 2;
        int kC4String = 3;
        int kC4Array = 4;
        int kC4Map = 5;
        int kC4EndSequence = 6;
        int kC4Special = 7;
        int kC4Error = 255;
    }
}
