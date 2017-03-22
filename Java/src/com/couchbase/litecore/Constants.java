//  Copyright (c) 2015-2016 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

package com.couchbase.litecore;

/**
 * Created by hideki on 9/30/15.
 */
public interface Constants {

    ////////////////////////////////////
    // c4Database.h
    ////////////////////////////////////

    // Document versioning system (also determines database storage schema)
    interface C4DocumentVersioning{
        int kC4RevisionTrees = 0;///< CouchDB and Couchbase Mobile 1.x revision trees
        int kC4VersionVectors = 1;///< Couchbase Mobile 2.x version vectors
    }

    ////////////////////////////////////
    // fdb_errors.h
    ////////////////////////////////////

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
    // Note: Superset of DocumentFlags
    interface C4DocumentFlags {
        int kDeleted = 0x01;        // The document's current revision is deleted.
        int kConflicted = 0x02;     // The document is in conflict.
        int kHasAttachments = 0x04; // One or more revisions have attachments.

        int kExists = 0x1000;       // The document exists (i.e. has revisions.)
    }

    // Flags that apply to a revision.
    // Note: Same as Revision::Flags
    interface C4RevisionFlags {
        int kRevDeleted = 0x01;        // Is this revision a deletion/tombstone?
        int kRevLeaf = 0x02;           // Is this revision a leaf (no children?)
        int kRevNew = 0x04;            // Has this rev been inserted since decoding?
        int kRevHasAttachments = 0x08; // Does this rev's body contain attachments?
        int kRevKeepBody = 0x10;       // Revision's body should not be discarded when non-leaf
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

    // Error domains:
    interface C4ErrorDomain {
        int LiteCoreDomain = 1;     // code is LiteCore-specific code (c4Base.h)
        int POSIXDomain = 2;        // code is an errno (errno.h)
        int ForestDBDomain = 3;     // code is a fdb_status (fdb_error.h)
        int SQLiteDomain = 4;       // code is a SQLite error (sqlite3.h)
    }

    // LiteCoreDomain error codes:
    interface LiteCoreError {
        int kC4ErrorAssertionFailed = 1;        // Internal assertion failure
        int kC4ErrorUnimplemented = 2;          // Oops, an unimplemented API call
        int kC4ErrorNoSequences = 3;            // This KeyStore does not support sequences
        int kC4ErrorUnsupportedEncryption = 4;  // Unsupported encryption algorithm
        int kC4ErrorNoTransaction = 5;          // Function must be called within a transaction
        int kC4ErrorBadRevisionID = 6;          // Invalid revision ID syntax
        int kC4ErrorBadVersionVector = 7;       // Invalid version vector syntax
        int kC4ErrorCorruptRevisionData = 8;    // Revision contains corrupted/unreadable data
        int kC4ErrorCorruptIndexData = 9;       // Index contains corrupted/unreadable data
        int kC4ErrorTokenizerError = 10;        // can't create text tokenizer for FTS
        int kC4ErrorNotOpen = 11;               // Database/KeyStore/index is not open
        int kC4ErrorNotFound = 12;              // Document not found
        int kC4ErrorDeleted = 13;               // Document has been deleted
        int kC4ErrorConflict = 14;              // Document update conflict
        int kC4ErrorInvalidParameter = 15;      // Invalid function parameter or struct value
        int kC4ErrorDatabaseError = 16;         // Lower-level database error (ForestDB or SQLite)
        int kC4ErrorUnexpectedError = 17;       // Internal unexpected C++ exception
        int kC4ErrorCantOpenFile = 18;          // Database file can't be opened; may not exist
        int kC4ErrorIOError = 19;               // File I/O error
        int kC4ErrorCommitFailed = 20;          // Transaction commit failed
        int kC4ErrorMemoryError = 21;           // Memory allocation failed (out of memory?)
        int kC4ErrorNotWriteable = 22;          // File is not writeable
        int kC4ErrorCorruptData = 23;           // Data is corrupted
        int kC4ErrorBusy = 24;                  // Database is busy/locked
        int kC4ErrorNotInTransaction = 25;      // Function cannot be called while in a transaction
        int kC4ErrorTransactionNotClosed = 26;  // Database can't be closed while a transaction is open
        int kC4ErrorIndexBusy = 27;             // View can't be closed while index is enumerating
        int kC4ErrorUnsupported = 28;           // Operation not supported in this database
        int kC4ErrorNotADatabaseFile = 29;      // File is not a database, or encryption key is wrong
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
