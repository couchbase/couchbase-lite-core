# LiteCore Class Overview

Last updated: 22 June 2021

## Diagram

<img src="Class%20Diagram.png" width="100%">

This diagram shows the relationships between the major LiteCore classes and C interfaces, both public and internal. It does not contain every class; there would be too many to fit. The text contains descriptions of more classes.

> **Looking for the replicator?** Its classes are documented on [a separate page][1].

- Shapes:
	- **Boxes**: C++ classes
	- **Ovals**: C interfaces (functions, structs, enums…)
- Lines:
	- **Solid**: “has a” (runtime relationship)
	- **Dashed**: “is a” (inheritance)
	- **Dotted**: “calls”
- Colors:
	- **Green**: Public API
	- **Yellow**: Internal classes
	- **Gray**: Abstract internal classes
	- **Purple**: 3rd party code



## 1. Introduction

LiteCore is a C++ library; it can be built as either static or dynamic. It presents a fairly flat object-oriented API, though it uses inheritance internally. The API is available either as C++ or C.

Some general architectural conventions within LiteCore:

* Errors are almost always thrown as exceptions, usually instances of `litecore::error`. But C++ exceptions are quite expensive to throw, so in some cases, if an error happens commonly in regular use (such as NotFound), it’s indicated as a special return value instead.
* These classes are in general **not** thread-safe, except where noted. A `C4Database` and all objects associated with it should be called only from a single thread at a time.
* Some, but not all, of these classes are reference-counted: subclasses of `fleece::RefCounted`. References to instances of ref-counted objects are smart pointers of class `fleece::Retained<T>`.




## 2. Public APIs

LiteCore exposes a fairly flat object-oriented API. The API classes (green rectangles) are C++ classes that mostly inherit from `fleece::RefCounted`. They’re in the global namespace, for C compatibility reasons, and prefixed with “`C4`”.

C++ is not a good language for implementing the APIs of dynamic libraries, due to issues like name-mangling, inlining and the fragile base class problem. Moreover, at least one of our platforms (.NET) can only bind to C APIs, not C++. So we expose a C version of the API as well. This is mostly a direct translation of the C++ API, where every class becomes an opaque struct and every method becomes a function whose first parameter is a pointer to the receiver. So for example the method `C4Widget::frob(int speed)` would become the C function `c4widget_frob(C4Widget*, int speed)`.

> Note: The C API is the original API; a public C++ API was only added in mid-2021. At the time of writing, all the clients use the C API.

The APIs are largely interchangeable, to C++ callers: each C++ class is the same type as the opaque C struct, so you can either call a method on it or pass it to a function.

The C API’s implementation is deliberately kept very thin: each function is simply a wrapper around the equivalent C++ method call, usually with a `try...catch` block to catch C++ exceptions and convert them to `C4Error` values. You can find these in the source files `c4CAPI.cc`, `c4Replicator_CAPI.cc`, and `REST_CAPI.cc`.



## 3. Database Subsystem

### C4Database

The top-level API class, corresponding to a Couchbase Lite `Database`.

It’s an abstract class, subclassed by `DatabaseImpl`. This is done just to avoid exposing all kinds of internal classes and types in a public header.

### C4Collection

Represents a Couchbase Lite `Collection`, a namespace of documents. (Documents will be described later.) A database contains collections, and collections contain documents. Indexes also belong to collections.

There is always a default collection, named "`_default`", and clients can create more.

Collections are conceptually contained in Scopes, but those aren't really present in the API. Instead a collection's identifier (`C4CollectionSpec`) is a pair consisting of its name and the name of its scope. The default scope is also named `_default`.

### SequenceTracker

Tracks an event stream of document changes in a collection, to support change notifications. It's informed of changes and commits by its owning `C4Collection`, as well as by `C4Collection` instances in any other `C4Database` instance on the same database file.

### CollectionChangeNotifier

An auxiliary class associated with a `SequenceTracker` that acts as bookmark on its event stream. A client that wants to observe changes to a specific document or to any document creates one of these. The `SequenceTracker` will call the client's callback function _once_, the first time a change occurs. The client can then call `readChanges()` to read the ordered list of collection changes. Only after the `readChanges()` call will the client become eligible for another callback.

> **NOTE:** The callback can happen on an arbitrary thread, so the client's handler needs to be sufficiently thread-safe.

This callback system reduces the overhead of posting notifications. It's expected that a client will have an event loop (or runloop or dispatch queue); the callback just needs to tell the scheduler to wake up the event loop. Then the handler running on the event loop can read all the changes at once.

### DocumentChangeNotifier

This class is similar to `CollectionChangeNotifier` but is used to receive notifications of changes to a single document. Since it won't be triggered as often, it doesn't have the same callback throttling behavior: it invokes its callback every time the document changes.

### Housekeeper and BackgroundDB

`Housekeeper` takes care of background operations like expiring documents. It runs on a background thread, and it uses the Database's `BackgroundDB`, a separate SQLite connection, to minimize blocking API calls.


## 4. Storage Subsystem

### DataFile

Abstract class representing a file managed by some storage engine (only SQLite, currently.) The concrete subclass `SQLiteDataFile` owns a SQLite3 database connection. Each `C4Database` owns a `DataFile` instance.

A `DataFile` acts as a container for named `KeyStore` objects, which store the actual data. There's one `KeyStore` for each collection; in general they're prefixed with "`coll_`", but for historical reasons the default one is named `default`.

There are also some `KeyStore`s used for auxiliary data. Each database has one named `info` for metadata such as the maximum rev-tree depth. The replicator uses another to store checkpoints.

### KeyStore

An abstract key/value store belonging to a `DataFile` that maps key strings to `Record`s, which are blobs with some metadata like a revision ID, sequence number, and flags. It also maintains indexes.

Most importantly, KeyStores store the documents of Collections; each C4Collection maps to one KeyStore. There are a few other KeyStores that are used for non-document storage, such as replicator checkpoints and the database's UUIDs.

The KeyStore of the default collection is named "`default`". 

The concrete subclass `SQLiteKeyStore` owns a single table in the database, whose name is prefixed with “`kv_`”. A database's default KeyStore is a table named "`kv_default`", a collection named "widgets" would have a table named "`kv_.widgets`", etc.

### Record

An item stored in a `KeyStore`. Record is a fairly "dumb" value type containing:

| Property    | Type   | Purpose |
|:------------|:-------|:--------|
| key         | string | The document-ID or primary key |
| body        | blob   | The document body (properties encoded as Fleece) |
| extra       | blob   | Secondary value; stores conflicting revisions and the revision history |
| version     | blob   | Identifies the revision; may store a revision-ID or version vector |
| sequence    | int    | Chronological sequence number in the KeyStore; updated automatically when the Record is saved |
| subsequence | int    | Incremented when Record changes in minor ways. |
| expiration  | int    | If not `NULL`, a timestamp of when this Record is to be purged (removed) |
| flags       | int    | (see below) |

The storage subsystem itself (`Record`, `KeyStore`, `DataFile`) doesn't care what's in the `body`, `extra`, `version` or `flags`; those are interpreted by higher levels.

| Flag Name      | Meaning |
|:---------------|:--------|
| deleted        | Is the current revision a deletion tombstone? |
| conflicted     | Is this document in conflict? |
| hasAttachments | Does any stored revision contain blobs? |
| synced         | Has the current revision been pushed to the server? |

### RecordUpdate

An even dumber cousin of `Record` that's used to pass updates into the `KeyStore` in a lighter-weight way. The main difference is that the string and blob properties are not required to be heap blocks (`alloc_slice`s.)



## 5. Query Subsystem

### C4Query

The public query class. Instantiated by a `C4Database`. Delegates most of its functionality to a `Query`...

### Query

Abstract database query object associated with, and created by, a `DataFile`. A `Query` is created from a [JSON][2] or N1QL query description, and stores its compiled form. Reusing a `Query` is more efficient than re-creating it. To run a `Query`, call `createEnumerator()`, passing in the parameter bindings if any.

As you'd expect, there is a concrete `SQLiteQuery` subclass.

There is a [separate document describing how we run N1QL queries in SQLite](QueryRuntime.md).

### C4QueryEnumerator

Delegates to a `QueryEnumerator`, an abstract iterator over a `Query`'s results. Starts out positioned _before_ the first row; call `next()` to advance to the next row, until it returns `false` at the end.

The `columns` property returns a Fleece array iterator that can be used to access the result columns.

The `SQLiteQueryEnumerator` subclass manages a `sqlite3_stmt` cursor. It tries to read individual rows from the cursor when `next()` is called, to keep RAM usage low, but this is only possible if no changes are made to the SQLite3 database on this connection. If a change is about to be made (i.e. a transaction is being opened), the enumerator detects this and quickly buffers the remaining rows into memory, then closes the cursor. This is invisible from the outside; the only side effect is increased memory usage.

### QueryTranslator

This class is only loosely connected, having no dependencies on the other classes described here. Its job is to convert  query description in JSON (actually a parsed Fleece object tree) into an equivalent SQLite `SELECT` statement. It acts like a simple compiler, first translating the JSON to an abstract syntax tree (AST), then traversing the tree to write SQL.

QueryTranslator also generates the `CREATE INDEX` statements used to create indexes.

To avoid dependencies, QueryTranslator uses a `Delegate` interface to describe the information it needs to know, like the names of tables corresponding to collections and FTS indexes. The interface is implemented by `DatabaseImpl`.

There is a [separate document describing the QueryTranslator](QueryTranslator.md).

### N1QL

N1QL queries are converted by a parser generated by the [PEG][3] tool from a high-level grammar. The output of the parser is a Fleece object tree corresponding to the JSON query schema; this is passed directly into the QueryTranslator.


## 6. Blob / Attachment Subsystem

### C4BlobStore

A pretty simple implementation of a content-addressable set of Blob objects, implemented as a directory of blob files, each of which is named after the SHA-1 digest of its contents.

### C4WriteStream, etc.

Stream classes for writing a blob. `C4WriteStream` is a public wrapper around the abstract base class `BlobWriteStream`. `FileWriteStream` simply uses the ANSI C library's file I/O functions, and `EncryptedWriteStream` delegates to another `WriteStream` and encrypts the data written to it (of which more below.)

New blobs are initially written to a temporary file. As the data is appended, the stream keeps a running tally of its SHA-1 digest. When complete, the caller calls `install()`, which causes the temporary file to be moved into the BlobStore directory under a name derived from its digest. Or if the stream is closed without calling `install()`, the temporary file is deleted.

### C4ReadStream, etc.

Seekable stream classes for reading a `Blob`'s data. (`C4BlobStore:: getContents()` is just a convenience that reads all the data from a `ReadStream` into memory.)

Again: `C4ReadStream` is a public wrapper, `ReadStream` is abstract, `FileReadStream` just calls ANSI C file functions, and `EncryptedReadStream` decrypts the data from another `ReadStream`.

#### Encrypted Blob Format

Unencrypted blob files don't have a file format: they contain nothing but the actual data. Encrypted blobs are more complicated.

We encrypt blobs (like databases) using the very widely used [AES][4] cipher with 256-bit keys (AES-256). This entails some work, because

* AES is a block cipher: the algorithm operates only on small fixed-size (32-byte) blocks of data.
* The common way to encrypt longer data is "cipher block chaining", but this doesn't support random access: you always have to start decrypting at the start of the data.
* Secure use of AES requires that you never encrypt two messages (files) using exactly the same key. (The reason is complicated. Trust me on this.)

So here’s the file format:

The _last_ 32 bytes of the file contain an unencrypted "nonce", which is a randomly-chosen value that's XORed with the database’s encryption key to create a _file key_ used only for this file.

The rest of the file is structured as a list of 4k-byte blocks, the last of which will be partial unless the blob's length is an exact multiple of 4096.

Each 4k block is encrypted using the file’s key using CBC. The "initialization vector" that's also passed to the CBC algorithm consists of the block's number in big-endian format. This way any block can be decoded independently. No padding is used, so the encrypted block is the same size as the original (4096 bytes), ensuring that blocks are efficiently aligned to filesystem block boundaries and the file size doesn't grow.

The final block is of course usually less than 4k in size; this one has to be encoded specially. The comment at the top of `EncryptedStream.cc` gives the gory details.


## 7. Document Subsystem

### DocumentFactory

Abstract class that vends `C4Document` instances; different subclasses create different `C4Document` subclasses. This allows for multiple types of document storage in `Record`s. Each `C4Collection` instance has one of these.

The two concrete subclasses are `TreeDocumentFactory` and `VectorDocumentFactory`. The choice of which to use is based on a flag in the `C4DatabaseConfig`.

### C4Document

Public, abstract class that provides access to a Couchbase Lite document and its revisions, using a `Record` as the backing store. 

The concrete subclasses are `TreeDocument` and `VectorDocument`.

### TreeDocument

A `C4Document` subclass that uses revision trees for versioning.

> **NOTE:** The classes below should only be accessed by TreeDocument; any code outside the Document subsystem should always go through `C4Document`.

> **NOTE:** CBL 4.0 and later no longer use `TreeDocument`, so all new and updated documents will have version vectors. However, pre-existing documents that haven't been updated by CBL 4 will still be in the old rev-tree format, so the subsidiary classes `RevTree` etc. are still needed to read from the old format.

#### RevTreeRecord

This class provides persistent storage of a `RevTree` in a `Record`.

#### RevTree

An in-memory tree of `Rev` objects, which can be serialized and de-serialized. One revision is always current. Supports the usual operations like inserting a revision with its history, purging, pruning, etc.

#### Rev

Represents a document revision:

| Property      | Type    | Purpose |
|---------------|---------|---------|
| parent        | `Rev*`  | Parent revision, or null |
| body          | `slice` | The content of the revision, if still available |
| sequence      | `int64` | Sequence number of doc when this revision was saved |
| revID         | `revID` | Revision ID, stored in `RevID` (compressed) form |
| deleted       | `bool`  | Is this a deletion tombstone? |
| leaf          | `bool`  | Is this a leaf (current) revision? |
| hasAttachments| `bool`  | Does the body contain blob references? |
| keepBody      | `bool`  | Should the body be preserved when this revision ceases to be a leaf? |
| isConflict    | `bool`  | If true, this is an unresolved conflicting revision |
| foreign       | `bool`  | Was revision pulled from a peer by the replicator? |

#### RawRevTree

Serializes a `RevTree` to/from binary format that's stored in a `Record`'s `extra` property.

### VectorDocument, VectorRecord

`VectorDocument` is a `C4Document` subclass that uses version vectors.

`VectorRecord` is a lower-level data model for `VectorDocument`, that stores its state in a `Record`. It represents a mapping from `RemoteID` (an integer) to `Revision` objects. Each `Revision` has document properties (a Fleece dict), a version vector, and flags.

* The `RemoteID` 0, called `Local`, represents the local document version.
* Other `RemoteIDs` are assigned to different remote databases (the replicator assigns these numbers.) Thus, a revision with a nonzero `RemoteID` represents the last known revision at that remote database. It may or may not contain a body.

`VectorRecord` stores its state in a `Record`. The local revision goes in the `body` and `version`. Others, if any, are stored in `extra` as a serialized Fleece structure. This structure has some optimizations to save space if revisions contain common properties; see the comments in `VectorRecord.cc` for details.

### revid

A binary-encoded revision ID, either the rev-tree kind (generation and digest) or a version vector. The encoding cuts the size in half. `RevID` is what’s stored in a `Record`’s `version`.

- A rev-tree revID encodes its generation as a varint, followed by the digest in binary form. So “3-abcd” would be encoded as (hex) `03ABCD`.
- A version vector is encoded as a zero byte followed by a sequence of versions, each of which is the generation as a varint followed by the peer ID as a varint. (The peer ID for the local database is represented as 0, to save space.)

> Note: `RevID` and its encoding are internal. `C4Document`’s public API exposes revision IDs as ASCII.

### VersionVector

A decoded version vector represented as a `std::vector<Version>`, where `Version` is a tuple of a generation number and a peer ID (each 64 bits.) This is used only by `VectorDocument` and `VectorRecord`.

[1]:	Replicator.md
[2]:	https://github.com/couchbase/couchbase-lite-core/wiki/JSON-Query-Schema
[3]:	http://github.com/snej/peg/
[4]:	https://en.wikipedia.org/wiki/Advanced_Encryption_Standard
