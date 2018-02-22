//
// C4Constants.java
//
// Copyright (c) 2017 Couchbase, Inc All rights reserved.
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
package com.couchbase.litecore;

public interface C4Constants {
    ////////////////////////////////////
    // c4Base.h
    ////////////////////////////////////
    interface C4LogLevel {
        int kC4LogDebug = 0;
        int kC4LogVerbose = 1;
        int kC4LogInfo = 2;
        int kC4LogWarning = 3;
        int kC4LogError = 4;
        int kC4LogNone = 5;
    }

    interface C4LogDomain {
        String Database = "DB";
        String Query = "Query";
        String Sync = "Sync";
        String WebSocket = "WS";
        String BLIP = "BLIP";
    }

    ////////////////////////////////////
    // c4Database.h
    ////////////////////////////////////

    // Boolean options for C4DatabaseConfig
    interface C4DatabaseFlags {
        int kC4DB_Create = 1;           ///< Create the file if it doesn't exist
        int kC4DB_ReadOnly = 2;         ///< Open file read-only
        int kC4DB_AutoCompact = 4;      ///< Enable auto-compaction
        int kC4DB_SharedKeys = 0x10;    ///< Enable shared-keys optimization at creation time
        int kC4DB_NoUpgrade = 0x20;     ///< Disable upgrading an older-version database
        int kC4DB_NonObservable = 0x40; ///< Disable c4DatabaseObserver
    }

    // Document versioning system (also determines database storage schema)
    interface C4DocumentVersioning {
        int kC4RevisionTrees = 0;   ///< CouchDB and Couchbase Mobile 1.x revision trees
        int kC4VersionVectors = 1; ///< Couchbase Mobile 2.x version vectors
    }

    // Encryption algorithms.
    interface C4EncryptionAlgorithm {
        int kC4EncryptionNone = 0;      ///< No encryption (default)
        int kC4EncryptionAES128 = 1;    ///< AES with 128-bit key
        int kC4EncryptionAES256 = 2;    ///< AES with 256-bit key
    }

    // Encryption key sizes (in bytes).
    interface C4EncryptionKeySize {
        int kC4EncryptionKeySizeAES128 = 16;
        int kC4EncryptionKeySizeAES256 = 32;
    }

    ////////////////////////////////////
    // c4Document.h
    ////////////////////////////////////

    // Flags describing a document.
    // Note: Superset of DocumentFlags
    interface C4DocumentFlags {
        int kDocDeleted = 0x01;        // The document's current revision is deleted.
        int kDocConflicted = 0x02;     // The document is in conflict.
        int kDocHasAttachments = 0x04; // One or more revisions have attachments.
        int kDocExists = 0x1000;       // The document exists (i.e. has revisions.)
    }

    // Flags that apply to a revision.
    // Note: Same as Revision::Flags
    interface C4RevisionFlags {
        int kRevDeleted = 0x01;        // Is this revision a deletion/tombstone?
        int kRevLeaf = 0x02;           // Is this revision a leaf (no children?)
        int kRevNew = 0x04;            // Has this rev been inserted since decoding?
        int kRevHasAttachments = 0x08; // Does this rev's body contain attachments?
        int kRevKeepBody = 0x10;       // Revision's body should not be discarded when non-leaf
        int kRevIsConflict = 0x20; ///< Unresolved conflicting revision; will never be current
        int kRevIsForeign = 0x40; ///< Rev comes from replicator, not created locally
    }

    ////////////////////////////////////
    // c4DocEnumerator.h
    ////////////////////////////////////

    // Flags for document iteration
    interface C4EnumeratorFlags {
        int kC4Descending = 0x01;
        int kC4IncludeDeleted = 0x08;
        int kC4IncludeNonConflicted = 0x10;
        int kC4IncludeBodies = 0x20;

        int kC4Default = kC4IncludeNonConflicted | kC4IncludeBodies;
    }


    ////////////////////////////////////
    // c4Query.h
    ////////////////////////////////////

    // Types of indexes.
    interface C4IndexType {
        int kC4ValueIndex = 0; ///< Regular index of property value
        int kC4FullTextIndex = 1; ///< Full-text index
        int kC4GeoIndex = 2; ///< Geospatial index of GeoJSON values (NOT YET IMPLEMENTED)
    }

    ////////////////////////////////////
    // c4Base.h
    ////////////////////////////////////

    // Error domains:
    interface C4ErrorDomain {
        int LiteCoreDomain = 1;     // code is a Couchbase Lite Core error code (see below)
        int POSIXDomain = 2;        // code is an errno (errno.h)
        /*int ForestDBDomain = 3;*/ // domain 3 is unused
        int SQLiteDomain = 4;       // code is a SQLite error (sqlite3.h)
        int FleeceDomain = 5;       // code is a Fleece error
        int NetworkDomain = 6;      // code is a network error code from the enum below
        int WebSocketDomain = 7;    // code is a WebSocket close code (1000...1015) or HTTP error (400..599)
        int kC4MaxErrorDomainPlus1 = 8;
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
        int kC4ErrorRemoteError = 35;           // Unknown error from remote server
        int kC4ErrorDatabaseTooOld = 36;        // Database file format is older than what I can open
        int kC4ErrorDatabaseTooNew = 37;        // Database file format is newer than what I can open
        int kC4ErrorBadDocID = 38;              // Invalid document ID
        int kC4ErrorCantUpgradeDatabase = 39;   // Database can't be upgraded (might be unsupported dev version)

        int kC4NumErrorCodesPlus1 = 40;         //
    }

    /**
     * Network error codes (higher level than POSIX, lower level than HTTP.)
     */
    // (These are identical to the internal C++ NetworkError enum values in WebSocketInterface.hh.)
    interface NetworkError {
        int kC4NetErrDNSFailure = 1;        // DNS lookup failed
        int kC4NetErrUnknownHost = 2;       // DNS server doesn't know the hostname
        int kC4NetErrTimeout = 3;
        int kC4NetErrInvalidURL = 4;
        int kC4NetErrTooManyRedirects = 5;
        int kC4NetErrTLSHandshakeFailed = 6;
        int kC4NetErrTLSCertExpired = 7;
        int kC4NetErrTLSCertUntrusted = 8;       // Cert isn't trusted for other reason
        int kC4NetErrTLSClientCertRequired = 9;
        int kC4NetErrTLSClientCertRejected = 10; // 10
        int kC4NetErrTLSCertUnknownRoot = 11;    // Self-signed cert, or unknown anchor cert
    }
}
