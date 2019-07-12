# LiteCore Class Overview

<img src="Class%20Diagram.png" width=584>

These are all C++ classes, and are all considered internal to LiteCore. Their APIs are subject to change. C++ is not a good language for implementing stable public APIs, due to issues like name-mangling, inlining and the fragile base class problem. LiteCore's public API is C-based and is a wrapper around these classes; clients should use it instead unless they're willing to statically link LiteCore and put up with API changes.

* Errors are thrown as exceptions, usually instances of `litecore::error`.
* These classes are **not** thread-safe, except where noted. A `Database` and all objects associated with it should be called only from a single thread at a time.
* Some, but not all, of these classes are reference-counted (subclasses of `RefCounted`).
* For the most part you don't instantiate these classes yourself, but get them from the `Database`.

> **Looking for the replicator?** Its classes are documented on [a separate page](Replicator.md).

## Database Subsystem

### Database

The top-level class, corresponding to a Couchbase Lite `Database`.

(There is a subclass `C4Database` but it's used only in the C API implementation, as a convenience.)

### SequenceTracker

Tracks an event stream of database changes, to support change notifications. It's informed of changes and commits by the Database class.

>**NOTE:** This class is thread-safe, because it can be called by any `Database` instance on the same physical database file, and these may be on different threads.

### DatabaseChangeNotifier

An auxiliary class associated with a `SequenceTracker` that acts as bookmark on its event stream. A client that wants to observe changes to a specific document or to any document creates one of these. The `SequenceTracker` will call the client's callback function _once_, the first time a change occurs. The client can then call `readChanges()` to read the ordered list of database changes. Only after the `readChanges()` call will the client become eligible for another callback.

>**NOTE:** The callback can happen on an arbitrary thread, so the client's handler needs to be sufficiently thread-safe.

This callback system reduces the overhead of posting notifications. It's expected that a client will have an event loop (or runloop or dispatch queue); the callback just needs to tell the scheduler to wake up the event loop. Then the handler running on the event loop can read all the changes at once.

### DocumentChangeNotifier

This class is similar to `DatabaseChangeNotifier` but is used to receive notifications of changes to a single document. Since it won't be triggered as often, it doesn't have the same callback throttling behavior: it invokes its callback every time the document changes.

## Storage Subsystem

### DataFile

Abstract class representing a file managed by some storage engine (SQLite, currently.) There is a concrete subclass for each storage engine, i.e. `SQLiteDataFile`.

DataFile acts as a container for named KeyStores, which store the actual data. At present the `Database` class uses one `KeyStore` named `default` for documents, and one named `info` for metadata such as the maximum rev-tree depth. The replicator uses another to store checkpoints.

### KeyStore

An abstract key/value store that maps blobs (keys) to `Record`s, which are blobs with some metadata. 

`KeyStore` also acts as a factory for `Query` objects, and has API for maintaining search indexes.

### Record

An item stored in a `KeyStore`. Record is a fairly "dumb" value type containing:

| Property | Type | Purpose |
|----------|------|---------|
| key | slice | The key used to retrieve the Record |
| body | slice | The primary value |
| sequence | integer | Chronological sequence number in the KeyStore; updated automatically when the Record is saved |
| version | slice | Identifies the revision; may store a revision-ID or version vector |
| deleted | bool | Is the current revision a deletion tombstone? |
| conflicted | bool | Is this document in conflict? |
| hasAttachments | bool | Does the current revision contain blobs? |
| synced | bool | Has the current revision been pushed to the server? |

The storage subsystem (`Record`, `KeyStore`, `DataFile`) only pays attention to `key` and `sequence`. The other properties are persisted but otherwise ignored at this level.

## Query Subsystem

### Query

Abstract database query object associated with, and created by, a `KeyStore`. A `Query` is created from a [JSON query description](https://github.com/couchbase/couchbase-lite-core/wiki/JSON-Query-Schema) and stores its compiled form. Reusing a `Query` is more efficient than re-creating it. To run a `Query`, call `createEnumerator()`, passing in the parameter bindings if any.

### QueryEnumerator

An iterator over a `Query`'s results. Starts out positioned _before_ the first row; call `next()` to advance to the next row, until it returns `false` at the end.

The `columns` property returns a Fleece array iterator that can be used to access the result columns.

A QueryEnumerator may be "live" or "prerecorded". A live enumerator retrieves more results from the database when the `next` method is called. A prerecorded one collects all the results in memory as soon as it's created. Obviously a live enumerator is more memory-efficient, while a prerecorded one supports random access to rows. The present SQLite implementation always uses prerecorded enumerators, because SQLite (as of 3.20) does not allow a connection to modify the database while there are active statements (cursors).

> **NOTE:** This class and its API have some technical debt to clean up. They're based too much on the Couchbase Lite 1.x API and assume that a row usually refers to a document; thus the Record-like properties `recordID`, `sequence`, etc. The CBL 2 query API is more SQL/N1QL-like and has a freeform set of columns defined by the query.

### QueryParser

This class is only loosely connected, having no dependencies on the other classes described here. Its job is to convert a JSON query description into an equivalent SQLite `SELECT` statement. To do this it recursively traverses the JSON (actually it converts it to a Fleece object tree and traverses that), writing the SQL output to a C++ `stringstream`. It uses some tables (declared in `QueryParserTables.hh`) to map operation names to handler methods and SQL functions.

## Blob / Attachment Subsystem

### BlobStore

A pretty simple implementation of a content-addressable set of Blob objects, implemented as a directory of blob files.

### Blob

Represents a persistent data blob, with a key derived from a SHA-1 digest of the contents. It really just stores the filesystem path, the key, and a pointer back to its BlobStore.

### WriteStream, etc.

Stream classes for writing a blob. The base class is mostly abstract, while `FileWriteStream` is a straightforward wrapper around the ANSI C library's file I/O functions, and `EncryptedWriteStream` delegates to another `WriteStream` and encrypts the data written to it (of which more below.)

New blobs are initially written to a temporary file. As the data is appended, the stream keeps a running tally of its SHA-1 digest. When complete, the caller calls `install()`, which causes the temporary file to be moved into the BlobStore directory under a name derived from its digest. Or if the stream is closed without calling `install()`, the temporary file is deleted.

### ReadStream, etc.

Stream classes for reading a `Blob`'s data. (`Blob::contents()` is just a convenience that reads all the data from a `ReadStream` into a slice.)

Again, `ReadStream` is abstract, `FileReadStream` just calls ANSI C file functions, and `EncryptedReadStream` decrypts the data from another `ReadStream`.

#### Encrypted Blob Format

Unencrypted blob files don't have a file format: they contain nothing but the actual data. Encrypted blobs are more complicated.

We encrypt blobs (like databases) using the very widely used [AES](https://en.wikipedia.org/wiki/Advanced_Encryption_Standard) cipher with 256-bit keys (AES-256). This entails some work, because

* AES is a block cipher: the algorithm operates only on small fixed-size (32-byte) blocks of data.
* The common way to encrypt longer data is "cipher block chaining", but this doesn't support random access: you always have to start decrypting at the start of the data.
* Secure use of AES requires that you never encrypt two messages (files) using exactly the same key. (The reason is complicated. Trust me on this.)

The _last_ 32 bytes of the file contain a "nonce", which is a randomly-chosen value that's XORed with the database encryption key to provide a key for this file. (No, the nonce isn't encrypted.)

The rest of the file is structured as a list of 4k-byte blocks, the last of which will be partial unless the blob's length is an exact multiple of 4096.

Each 4k block is encrypted using the file key using CBC. The "initialization vector" that's also passed to the CBC algorithm consists of the block's number in big-endian format. This way any block can be decoded independently. No padding is used, so the encrypted block is the same size as the original (4096 bytes), ensuring that blocks are efficiently aligned to filesystem block boundaries and the file size doesn't grow.

The final block is of course usually less than 4k in size; this one has to be encoded specially. The comment at the top of EncryptedStream.cc gives the gory details.

## Document Subsystem

### DocumentFactory

Abstract class that vends `Document` objects; different classes create different `Document` subclasses. This allows for multiple types of document storage in `Record`s. We currently use one based on revision-trees (`TreeDocumentFactory`), but there's another currently-dormant one that uses version vectors.

### Document

Abstract class that provides access to a Couchbase Lite document and its revisions, using a `Record` as the backing store. The currently used subclass is `TreeDocument`, which uses `VersionedDocument` under the hood.

>**NOTE:** The classes below should only be accessed by TreeDocument; any code outside the Document subsystem should always go through `Document`.

### VersionedDocument

This class provides persistent storage of a `RevTree` in a `Record`.

### RevTree

An in-memory tree of `Rev` objects, which can be serialized and de-serialized. One revision is always current. Supports the usual operations like inserting a revision with its history, purging, pruning, etc.

### Rev

Stores a document revision:

| Property | Type | Purpose |
|----------|------|---------|
| parent | Rev* | Parent revision, or null |
| body | slice | The content of the revision, if still available |
| sequence | integer | Sequence number of doc when this revision was saved |
| revID | revID | Revision ID, stored in compressed form |
| deleted | bool | Is this a deletion tombstone? |
| leaf | bool | Is this a leaf (current) revision? |
| hasAttachments | bool | Does the body contain blob references? |
| keepBody | bool | Should the body be preserved when this revision ceases to be a leaf? |
| isConflict | bool | If true, this is an unresolved conflicting revision |
| foreign | bool | Was revision pulled from a peer by the replicator? |
