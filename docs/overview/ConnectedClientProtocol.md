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

Request:

* `id`: Document ID
* `ifNotRev`: (optional) If present, and its value is equal to the document’s current revision ID, the peer SHOULD respond with a HTTP/304 error instead of sending the revision

Response:

* `rev`: The current revision ID
* `deleted`: (optional) Set to  `true` if the document is deleted and this revision is a “tombstone”
* Body: The current revision body as JSON

### 3.2. `getAttachment`

Exactly as in the replicator protocol.

### 3.3. `putRev`

Uploads a new revision to the peer.

The properties and behavior are identical to the replicator protocol’s `rev` message. The reason for a new message type is because the LiteCore replicator assumes that incoming `rev` messages are caused by prior `changes` responses, and would become confused if it received a standalone `rev` message. This made it apparent that it would be cleaner to treat this as a separate type of request.

Request: *same as existing `rev` message*

Response: *same as existing `rev` message* (never sent no-reply)

> **Note:** As usual, the receiving peer may send one or more getAttachment requests back to the originator if it needs the contents of any blobs/attachments in the revision.

### 3.4. `subChanges`

As in the replicator protocol, with one addition: the request’s `since` property may have a value of “`NOW`”. The receiving peer MUST interpret this as equal to the database/bucket’s latest sequence number. This causes only future changes to be sent.

> **Note**: This value is always used along with the `continuous` property, since otherwise no changes would be returned.

### 3.5. `unsubChanges`

This terminates the effect of `subChanges`: the receiving peer MUST stop sending `changes` messages as soon as possible.

_(No request properties or body defined.)_
