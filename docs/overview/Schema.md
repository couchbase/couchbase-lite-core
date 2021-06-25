# LiteCore's SQLite Database Schema

This document describes how LiteCore stores and indexes data in its underlying SQLite database. 

**You don't need to know any of this unless you work on LiteCore's storage subsystem, or want to troubleshoot a database at a very low level.** If you just want to inspect a database, use the [`cblite`](https://github.com/couchbaselabs/cblite) command-line tool. The `sqlite3` tool isn't very useful, even with the knowledge found below, because (a) most of the interesting data is encoded in binary formats, and (b) most mutating operations will fail because they update indexes and invoke triggers that use custom SQLite functions not available outside LiteCore.


## 1. DataFiles

LiteCore's low-level storage layer manages **DataFile**s, which support multiple **KeyStores**, each of which contains **Record**s.

> The C++ classes `DataFile` and `KeyStore` are abstract, meant to be independent of storage engine. The concrete subclasses are `SQLiteDataFile` and `SQLiteKeyStore`.

A DataFile is implemented as a SQLite database file, named `db.sqlite3`, stored in the database's `.cblite2/` directory.


## 2. KeyStores

Currently LiteCore creates and uses three KeyStores:

* `default` — Documents
* `info` — Various metadata values, like the database's UUIDs
* `checkpoints` — Replicator checkpoints

KeyStores are SQLite tables, whose names are prefixed with "`kv_`". So, `kv_default` is the table containing documents.

(In a 3.x database with multiple collections, each other collection has a KeyStore whose name is prefixed with `coll_`. So the `widgets` collection has a KeyStore named `coll_widgets`, which has a SQLite table named `kv_coll_widgets`.)

A KeyStore's table has the following SQL schema:

### In 2.x

| Column | Type | Description |
|--------|------|-------------|
| `key`    | text | Document ID, or other key (primary key) |
| `sequence` | integer | Sequence number |
| `flags` | integer | `DocumentFlags` |
| `version` | blob | `revid`: revision ID in compressed form |
| `body` | blob | `RawRevTree`: Binary revision tree, including revision bodies in Fleece format |
| `expiration` | integer | Timestamp of doc expiration, or NULL |

* The `expiration` column is only added the first time an expiration time is set on a document.

### In 3.0

| Column | Type | Description |
|--------|------|-------------|
| `key`    | text | Document ID, or other key (primary key) |
| `sequence` | integer | Sequence number |
| `flags` | integer | `DocumentFlags`, plus "subsequence" in high bits |
| `version` | blob | `revid`: revision ID or version vector in compressed form |
| `expiration` | integer | Timestamp of doc expiration, or NULL |
| `body` | blob | The current revision body, encoded as a Fleece Dict |
| `extra` | blob | Revision metadata, and bodies of other revisions |

* In a rev-tree database, `extra` contains the `RawRevTree` as in 2.x, just without the current revision's body.
* In a version-vector database, `extra` is Fleece-encoded; see `VectorRecord.cc` for details.

The separation of the current revision body from the other revision data makes it easier for queries to access the body: they can just parse the `body` column as Fleece. It also allows documents to avoid reading the extra data -- which may be larger than the body if it contains other revisions -- when they don't need it.

### Sequences

All KeyStores have a `sequence` column, but only the ones that store documents use it. The secondary KeyStores used for metadata (`info`, `checkpoints`, etc.) just ignore it.

Every time a revision is added to a document, its `sequence` property is updated to the next consecutive sequence number in that KeyStore, starting from 1.

There is a `kvmeta` table that just stores the latest sequence number of each KeyStore:
```
CREATE TABLE kvmeta (
    name TEXT PRIMARY KEY,
    lastSeq INTEGER DEFAULT 0 )  WITHOUT ROWID;
```

Most databases also contain a SQLite index named `kv_default_seqs`, which is created automatically the first time the KeyStore is iterated in sequence order (as during a push replication.)

In 3.0 a "subsequence" was added, in order to solve some limitations of MVCC. When a Record is updated and written back to the database (in a KeyStore that uses sequences) its sequence has to match the one on disk, or a conflict error is returned. But if in the interim the Record had been changed in a minor way that didn't bump its sequence -- like changing flags or expiration -- this test wouldn't detect the change, so it would be overwritten. To solve this, we added a "subsequence", a separate number that's normally zero, but is incremented when a document is updated without changing its sequence. The MVCC test on update now compares subsequences too, so it properly detects minor changes.

(As a minor optimization, the subsequence doesn't have its own column. Instead we stole the high-order bits of the `flags` column to store it.)

### Expiration

The first time any record in a KeyStore is given an expiration time (TTL), a new column `expiration` is added to its KeyStore's table to record it. This column contains a number (seconds since Unix epoch) in records that expire, and is null otherwise. An index `kv_default_expiration` is also created to allow efficient search of expired records.


## 3. Indexes

In the storage architecture, indexes belong to a KeyStore (not directly to a DataFile). Usually that means the `default` KeyStore, but in a database with multiple collections, each one's KeyStore can have indexes.

An index has a unique name within the DataFile. The discussion below describes the schema of an index named literally "`NAME`" on the default KeyStore.

### Value Indexes

A value index is simply a SQLite index named "`NAME`" on the table `kv_default`. Instead of indexing a SQL column, it indexes an expression, commonly a property accessor, which is translated to SQL from the original LiteCore JSON query syntax.

### Full-Text (FTS) Indexes

A full-text index is a SQLite FTS4 virtual table named `kv_default::NAME`:
```
CREATE VIRTUAL TABLE "kv_default::NAME" USING fts4("contact.address.street", tokenize=unicodesn);
```
(SQLite FTS4 also creates some real SQL tables for internal use, which are named after the virtual table with `_content`, `_segments`, etc. appended.)

LiteCore creates some SQL triggers on `kv_default` that update the FTS4 table when a record changes. These are named after the virtual table with `::ins`, `::upd`, `::del` appended.

### Array (UNNEST) Indexes

An array index creates a SQL table whose name begins `kv_default:unnest:`. This table contains a row for _each individual array element_ in every document that contains an array at that path.

```
CREATE TABLE "kv_default:unnest:PATH" (
    docid INTEGER NOT NULL REFERENCES kv_default(rowid),
    i INTEGER NOT NULL, 
    body BLOB NOT NULL,
    CONSTRAINT pk PRIMARY KEY (docid, i) )  WITHOUT ROWID;
```

* `docid` is a foreign key pointing to the source record (document).
* `i` is the array index where this element was found.
* `body` is the Fleece-encoded value of the array element.

LiteCore creates some SQL triggers on `kv_default` that update this table when a record changes. These are named after the index table with `::ins`, `::upd`, `::del` appended.

Last but not least, since the purpose of this table is to enable efficient array queries, it also has a regular SQL index:
```
CREATE INDEX "NAME" ON "kv_default:unnest:PATH" (fl_unnested_value(body));
```
or
```
CREATE INDEX "NAME" ON "kv_default:unnest:PATH" (fl_unnested_value(body, 'SUB_PROPERTY'));
```

(If there are multiple LiteCore indexes on the same path, but indexing different sub-properties, they share the same index table but of course create separate SQL indexes.)

### Predictive (ML) Indexes

A predictive index is much like an array index. Its name begins with `kv_default:predict:`.
```
CREATE TABLE "kv_default:predict:XXX" (
    docid INTEGER PRIMARY KEY REFERENCES kv_default(rowid),
    body BLOB NOT NULL ON CONFLICT IGNORE )  WITHOUT ROWID;
```

* `docid` is a foreign key pointing to the source record (document).
* `body` is the Fleece-encoded result of the prediction function.

LiteCore creates some SQL triggers on `kv_default` that update this table when a record changes. These are named after the predictive table with `::ins`, `::upd`, `::del` appended.

Lastly, there is a SQLite index on the predictive table, that indexes the desired result property:

```
CREATE INDEX "NAME" ON "kv_default:predict:DIGEST" (fl_unnested_value(body, 'RESULT_PROPERTY'));
```

(If there are multiple LiteCore indexes on the same prediction, but indexing different result properties, they share the same predictive table but of course create separate SQL indexes.)

### The `indexes` table

With the proliferation of index types, we've added a table to keep track of indexes. It's named `indexes` and has a row for each index; its columns are:

| Column | Type | Description |
|--------|------|-------------|
| `name` | text | the index name as registered through the API (Primary key) |
| `type` | integer | the index type, corresponding to the enums `KeyStore::IndexType` and `C4IndexType` |
| `keyStore` | text | the name of the KeyStore being indexed |
| `expression` | text | the JSON query expression describing the index |
| `indexTableName` | text | the name of the SQLite table created for the index |
