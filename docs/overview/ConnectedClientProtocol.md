# Connected Client Protocol

Couchbase Mobile 3.x

Jens Alfke — 11 April 2022

## 1. Introduction

The Connected Client protocol allows mobile clients to access a server-side database (bucket) directly, without needing a local database replica. In its first incarnation it supports document CRUD operations and database change listeners.

Sync Gateway already supports a REST API (inherited from CouchDB) that allows this, but our past experience with version 1.x showed that this API is difficult to support properly and inherits significant overhead from HTTP. As a result **we’ve chosen to implement Connected Client over BLIP, using an extension of the replicator protocol**.

This document describes those extensions.

## 2. Connecting

Opening a client connection is identical to opening a replicator connection. It’s the same WebSocket endpoint (`/dbname/_blipsync`), the same BLIP protocol, and the same authentication.

## 3. Message Types

These are the only messages the Connected Client implementation currently sends.

### 3.1. `getRev`

Requests a document’s current revision from the peer.

> **Note**: This is very much like the replicator protocol’s `rev` message, except that it’s sent as a *request* for a revision, not an announcement of one.

**Request**:

* `id`: Document ID
* `ifNotRev`: (optional) If present, and its value is equal to the document’s current revision ID, the peer SHOULD respond with a HTTP/304 error instead of sending the revision

**Response**:

* `rev`: The current revision ID
* Body: The current revision body as JSON

### 3.2. `getAttachment`

Exactly as in the replicator protocol.

### 3.3. `putRev`

Uploads a new revision to the peer.

The properties and behavior are identical to the replicator protocol’s `rev` message. The reason for a new message type is because the LiteCore replicator assumes that incoming `rev` messages are caused by prior `changes` responses, and would become confused if it received a standalone `rev` message. This made it apparent that it would be cleaner to treat this as a separate type of request.

**Request**: *same as existing `rev` message*

**Response**: *same as existing `rev` message* (never sent no-reply)

> **Note:** As usual, the receiving peer may send one or more getAttachment requests back to the originator if it needs the contents of any blobs/attachments in the revision.

### 3.4. `subChanges`

As in the replicator protocol, with one addition:

**Request**:

* `future`: (Optional) If `true`, the receiving peer MUST not send any existing sequences, only future changes. In other words, this has the same effect as a `since` property whose value is the current sequence ID. (The message SHOULD NOT also contain a `since` property, and the recipient MUST ignore it if present.)

> **Note**: `future` will always combined with `continuous`, since otherwise no changes would be sent at all!

### 3.5. `unsubChanges`

This terminates the effect of `subChanges`: the receiving peer MUST stop sending `changes` messages as soon as possible.

The sender MAY send another `subChanges` message later, to start a new feed.

_(No request properties or body defined.)_

### 3.6 `query`

Runs a query on the peer, identified by a name. Queries take zero or more named parameters, each of which is a JSON value.

Optionally, a server MAY allow a client to run arbitrary queries. (Sync Gateway will not support this for security and performance reasons, but Couchbase Lite applications can choose to.)

The result of a query is a list of rows, each of which is an array of column values. Each row has the same number of columns. Each column has a name.

**Request**:

* `name`: The name of the query (if named)
* `src`: N1QL or JSON query source (if not named)
* Body: A JSON object mapping parameter names to values

**Response**:

* Body: A JSON array:
  * The array MAY be empty if there were no query results. 
  * Otherwise its first item MUST be an array of column names, each of which MUST be a string. 
  * The remaining items are the rows of the query result. Each row MUST be an array with the same number of elements as the first item.

**Errors**:

- HTTP, 400 — Missing `name` or `src`, or both were given, or the body is not a JSON object, or a N1QL syntax error.
- HTTP, 403 — Arbitrary queries are not allowed
- HTTP, 404 — Query name is not registered

### 3.7 `allDocs`

Requests the IDs of all documents, or those matching a pattern.

Deleted documents are ignored.

**Request:**

- `idPattern`: (optional) A pattern for the returned docIDs to match. Uses Unix shell “glob” syntax, with `?` matching a single character, `*` matching any number of characters, and `\` escaping the next character.

**Response**:

- Body: A JSON array of docIDs. Each item is a string. The order of the docIDs is arbitrary.

**Errors:**

- HTTP, 400 — If patterns are not supported.
