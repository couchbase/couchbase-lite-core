
# LiteCore Replicator Overview

<img src="Replicator%20Diagram.svg" width=584>

These are all C++ classes, and are all considered internal to LiteCore. See [the main class overview page](index.md) for more caveats.

You may notice that the replicator classes don't communicate directly with the [database classes](index.md); instead, they go through the C API. This was originally done so that the replicator could be a separate shared library; we decided not to do that, but the separation is still useful architecturally.

## (Almost) Required Reading

To understand the replicator code, you really should read about the [replication protocol](https://github.com/couchbase/couchbase-lite-core/blob/master/modules/docs/pages/replication-protocol.adoc).

And to understand that protocol, it's useful to read about the [BLIP protocol](https://github.com/couchbaselabs/BLIP-Cocoa/blob/master/Docs/BLIP%20Protocol.md) that it's built on. But if you just want the tl;dr, it's that BLIP is a sort of RPC protocol that lets you send request & reply messages over a TCP socket; it's based on WebSockets but adds the ability to match messages with their replies, and to multiplex multiple messages simultaneously.

## Concurrency In The Replicator

The replicator code makes heavy use of the BLIP library's `Actor` class, which implements the [Actor Model](https://en.wikipedia.org/wiki/Actor_model) of concurrency. An Actor is an object that internally runs its own event loop; its public API is asynchronous, with all public method calls being delegated to the event loop, where they are processed serially. The key benefit is that the implementation of an Actor is single-threaded, so it's easy to reason about, while each Actor still runs on its own thread (conceptually) for high concurrency.

The BLIP-Cpp repo has an [overview of Actors](https://github.com/couchbaselabs/BLIP-Cpp/blob/master/docs/Actors.md). It covers both the general principles and how to use its implementation.

In `Actor` subclasses the convention is that public methods simply call `enqueue` to schedule processing; the actual implementation of each method is in a private method that has the same name but with an underscore prefix. These private methods run on the Actor's event queue, so _only one can run at a time_, making it safe to access the class's data members without mutexes.

Most of the replicator classes (`Replicator`, `Pusher`, `Puller`, `RevFinder`, `IncomingRev`, `IncomingBlob`, `Inserter`) are Actors. They're actually subclasses of an `Actor` subclass `Worker` which adds some common functionality they need. Most importantly, it organizes instances into a hierarchy for purposes of progress and error notification.

## Replicator Classes

### C4Replicator

A glue class that sits between `Replicator` and the public C API. (This class happens to be the implementation of the opaque `C4Replicator` struct declared in the C API.)

### Replicator

The top-level replicator class. Manages a _single_ connection to a peer; higher-level behaviors like retrying failed connections or waiting for connectivity are handled by the Couchbase Lite replicator implementation. This means that a single CBL Replicator object may instantiate multiple LiteCore `Replicator` objects during its lifetime, if it has to make multiple connections.

A new Replicator is given an existing `WebSocket` object, and in turn creates a `Connection` object _(q.v.)_ to manage it.

Beyond coordinating the `Puller` and `Pusher` objects, the Replicator also saves and loads checkpoints and sends events to its delegate.

### Puller

Manages the 'pull' side of a replication:

1. Receiving lists of document/revision IDs from the peer and identifying which ones are not in the database
2. Receiving document revision bodies, and identifying which referenced blobs are not in the database
3. Receiving blobs and saving them
4. Saving the documents (in bulk)
5. Updating the remote sequence number in the checkpoint

The Puller uses a `RevFinder` object to help with step 1, a pool of `IncomingRev` and `IncomingBlob` objects to manage steps 2 and 3, and an `Inserter` object to manage step 4. These are all Actors (subclasses of Worker) so they all run on independent threads for greater concurrency.

### Pusher

Manages the 'push' side of a replication:

1. Identifying all the revisions newer than the latest checkpoint, and sending them to the peer
2. Scheduling uploading the requested revisions to the peer
3. Uploading blobs to the peer on request
4. Updating the local sequence number in the checkpoint

### DBAccess

Coordinates thread-safe access to the database by the other replicator classes. The only way to obtain a reference to the `C4Database` handle is to call `DBAccess::use()`, which takes a lambda as a parameter; this method first locks an internal mutex, then calls the lambda passing the `C4Database` as a parameter. On return it unlocks the mutex.

`DBAccess` also has a number of methods for accessing documents, managing deltas, and inserting documents.

As an optimization, when `DBAccess` inserts documents it uses a second `C4Database` instance on the same physical database. This way other threads can continue to read from the database while a relatively time-consuming insertion is happening.

### C4SocketImpl

A subclass of `WebSocketImpl` (which itself subclasses the pure-virtual `WebSocket` interface.) It acts as glue between `WebSocketImpl` and the `C4SocketFactory` C API which provides the platform's actual network operations.

(Not shown in the diagram is `LoopbackWebSocket`, an entirely different implementation of `WebSocket` that simply passes the messages in memory between two instances of itself. This class is used for database-to-database replication, connecting two `Replicator` instances.)

## BLIP Classes

These classes are in the separate [BLIP-Cpp](https://github.com/couchbaselabs/BLIP-Cpp) repo, but in practice are inseparable from LiteCore. They have no dependencies on the replicator, but they do depend on the lower-level LiteCore support classes and on Fleece.

### Connection

The public API for a BLIP connection, which mostly acts as a façade around a `BLIPIO` object.

### BLIPIO

An `Actor` that communicates with a `WebSocket` instance to send and receive BLIP messages.

* Manages sets of outgoing and incoming BLIP messages
* Multiplexes outgoing messages, adding frame headers to each 4K (sometimes 16K) chunk
* Routes incoming chunks to the correct incoming messages
* Dispatches completed incoming messages to their handlers

### MessageIn

An incoming BLIP message. It's fed frames by the `BLIPIO` and reassembles them into the message body and headers.

### MessageOut

An outgoing BLIP message. Breaks the message into frames on demand by the `BLIPIO`.

(`MessageOut` is actually an internal class in BLIP-Cpp: the API uses the Builder pattern, so the replicator creates a `MessageBuilder` object and configures it; then the `Connection` internally creates a `MessageOut` from it.)

### WebSocket

Abstract API of a WebSocket connection: it has pure-virtual methods for connecting / disconnecting, and sending / receiving messages.

### WebSocketImpl

Implementation of `WebSocket` that knows how to serialize and deserialize WebSocket messages, and manage specific messages like PING and CLOSE, but still doesn't provide actual network transport.
