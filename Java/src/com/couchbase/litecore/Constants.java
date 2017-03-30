/**
 * Copyright (c) 2017 Couchbase, Inc. All rights reserved.
 * <p>
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
 * except in compliance with the License. You may obtain a copy of the License at
 * <p>
 * http://www.apache.org/licenses/LICENSE-2.0
 * <p>
 * Unless required by applicable law or agreed to in writing, software distributed under the
 * License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
 * either express or implied. See the License for the specific language governing permissions
 * and limitations under the License.
 */

package com.couchbase.litecore;

/**
 * Created by hideki on 9/30/15.
 */
public interface Constants {

    ////////////////////////////////////
    // c4Database.h
    ////////////////////////////////////

    // Document versioning system (also determines database storage schema)
    interface C4DocumentVersioning {
        int kC4RevisionTrees = 0;///< CouchDB and Couchbase Mobile 1.x revision trees
        int kC4VersionVectors = 1;///< Couchbase Mobile 2.x version vectors
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
        int kDescending = 0x01;
        int kInclusiveStart = 0x02;
        int kInclusiveEnd = 0x04;
        int kIncludeDeleted = 0x08;
        int kIncludeNonConflicted = 0x10;
        int kIncludeBodies = 0x20;

        int kDefault = kInclusiveStart | kInclusiveEnd | kIncludeNonConflicted | kIncludeBodies;
    }

    //////// INDEXES:

    // Types of indexes.
    interface C4IndexType {
        int kC4ValueIndex = 0; ///< Regular index of property value
        int kC4FullTextIndex = 1; ///< Full-text index
        int kC4GeoIndex = 2; ///< Geospatial index of GeoJSON values (NOT YET IMPLEMENTED)
    }

    //////// ERROR:

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
        int kC4ErrorWrongFormat = 30;           // Database exists but not in the format/storage requested
        int kC4ErrorCrypto = 31;                // Encryption/decryption error
        int kC4ErrorInvalidQuery = 32;          // Invalid query
        int kC4ErrorMissingIndex = 33;          // No such index, or query requires a nonexistent index
        int kC4ErrorInvalidQueryParam = 34;     // Unknown query param name, or param number out of range

    }
}
