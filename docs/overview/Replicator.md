# LiteCore Replicator Overview

Jens Alfke — 10 November 2021

This is a high level overview of the LiteCore 3.0 replicator architecture and most of its classes.


## (Almost) Required Reading

To understand the replicator code, you really should read about the [replication protocol](https://github.com/couchbase/couchbase-lite-core/blob/master/modules/docs/pages/replication-protocol.adoc).

And to understand that protocol, it's useful to read about the [BLIP protocol](../../Networking/BLIP/docs/BLIP%20Protocol.md) that it's built on. But if you just want the tl;dr, it's that BLIP is a sort of RPC protocol that lets you send request & reply messages over a TCP socket; it's based on WebSockets but adds the ability to match messages with their replies, and to multiplex multiple messages simultaneously.

The main replicator classes use a concurrency framework called [Actors](../../Networking/BLIP/docs/Actors.md), which has its own documentation. An `Actor` is an object that internally runs its own event loop; its public API is asynchronous, with all public method calls being delegated to the event loop, where they are processed serially. The key benefit is that the implementation of an Actor is single-threaded, so it's easy to reason about, while each Actor still runs on its own thread (conceptually) for high concurrency.

## Layering

There are two major layers of the replicator: an **internal implementation**, `litecore::Replicator`, and an **API implementation**, `C4Replicator`, that uses the internal one. The main reason for this is to implement reconnections: the internal implementation makes exactly one connection (one WebSocket) and can’t be restarted, while the API implementation can restart/reconnect by creating a new internal-replicator instance. 

The separation makes the design clearer and more reliable, since it would be difficult to restore all of the internal replicator’s state back to where it could connect again.

From top to bottom, the full layering looks like:

* _Platform replicator class_
* LiteCore replicator API (`C4Replicator`)
* Replicator internal implementation (`litecore::Replicator` etc.)
* BLIP protocol (`litecore::blip::Connection` etc.)
* WebSocket protocol (`litecore::websocket::WebSocketImpl`)
* WebSocket or TCP socket (_platform’s `C4SocketFactory`_, or else `litecore::websocket::BuiltInWebSocket`)

## Concurrency

The replicator is definitely the most complex part of LiteCore, and the largest source of issues. That’s primarily because of concurrency. Most of LiteCore is single-threaded by design, but the replicator needs concurrency to work efficiently, interleaving network I/O, computation, and file I/O. 

Unfortunately, concurrency is really hard. Taming it is an unsolved problem in computer science.

For the most part, the replicator is based on [Actors](../../Networking/BLIP/docs/Actors.md). This allows subcomponents to run single-threaded, which simplifies their logic. Our Actor implementation schedules each method-call on a private thread pool, or on Apple platforms it uses the system `dispatch_queue` API that’s backed by an OS-managed thread pool.

`Batcher` is a utility used by some Actors to allow them to group multiple requests into one. A Batcher has a queue to which objects can be added very efficiently. Periodically — when the queue grows large enough, or at some interval after the first item was added — the Batcher notifies its owner, which then pops the queue’s contents as a `vector`, then processes them together.

`access_lock` is a utility class template that wraps an object and allows only one caller at a time to obtain a reference to it. If another thread tries to obtain a reference, it blocks until it’s safe. (It’s used by `DBAccess`. I’m sure we could use it more widely, but it was developed relatively recently, after the replicator was first implemented. Perhaps future refactoring will broaden its use.)

A few other classes, notably `WebSocketImpl`, use mutexes directly.

`Timer` is a utility that lets clients schedule a callback after a certain delay or at a certain time. This utility runs a single background thread for scheduling, and invokes the callbacks on that thread. The callback thus needs to be thread-safe by using one of the above mechanisms.

## Replicator Classes

<img src="Replicator%20Diagram.svg" width=584\>

> Note: the diagram is slightly out of date. It’s missing some of the Pusher / Puller helper classes, and shows an obsolete class `IncomingBlob`.

These are all C++ classes, and are all considered internal to LiteCore. See [the main class overview page](index.md) for more caveats.


## API Classes

LiteCore’s public replicator API defines a class `C4Replicator`. Like the rest of the API, this is a C++ class (in `c4Replicator.hh`) that can also be used via C functions declared in `c4Replicator.h`.

C4Replicator is the abstract base class of an internal class hierarchy:

- `C4Replicator` — mostly-abstract interface
	- `C4ReplicatorImpl` — implementats most of C4Replicator’s API
		- `C4RemoteReplicator` — replication over the network
		- `C4LocalReplicator` — replication between two local databases
		- `C4IncomingReplicator` — a passive replicator connected to an incoming socket connection; used by `C4Listener`.

In addition to bridging to the public LiteCore API, these classes also manage reconnections. Even a one-shot replicator will reconnect if the initial connection fails with a transient error, and a continuous replicator will keep reconnecting indefinitely.

Similarly, they implement the “suspended” and “hostReachable” properties of the public API, which end up triggering disconnecting or reconnecting.

## Implementation Classes

The replicator implementation consists of a number of classes that work together. The main ones are:

- **Replicator**
	- DBAccess
	- Checkpointer
	- **Pusher**
		- ChangesFeed
	- **Puller**
		- **RevFinder**
		- **IncomingRev**
		- **Inserter**

There’s a single instance of these per replicator, except for IncomingRev.

Even though these are internal classes, they use the LiteCore API classes instead of the lower-level implementation ones, e.g. `C4Database` instead of `litecore::DataFile`. (But they do use some private API functions found in `c4Private.h.`) This keeps the architecture cleaner.

### Worker

The boldfaced classes are Actors; they’re actually subclasses of an abstract `Actor` subclass called `Worker`. The features Worker adds are:

- Access to the database, replicator options, BLIP connection.
- A hierarchy, with Replicator at the top, matching the outline shown above. (Each Worker has a reference to its parent object.)
- A `C4ReplicatorStatus` value, which stores the busy/idle/stopped status, current and total progress, and possibly an error.
- Passing status and progress changes up the hierarchy.

### Replicator

This is the top-level class that takes care of setup, connection management, and communication with its Delegate. 

As the root of the Worker tree, it gets notified about all progress changes and errors, aggregates them, and passes them on to the delegate.

### DBAccess

`DBAccess` arbitrates access to a single database instance by the other classes. Using `access_lock`, it allows only one thread at a time to obtain a reference to the `C4Database`.

It also maintains a second database instance that’s used only for writes. This improves performance, since the main database instance is still available while a write transaction is in progress.

DBAccess also implements some higher-level operations used by the other classes, to keep those classes’ implementations simpler.

### Checkpointer

`Checkpointer` manages the replication checkpoint. On behalf of the Puller it tracks the latest remote sequence ID, and on behalf of the Pusher it tracks which local sequences have been pushed.

On startup it reads the saved checkpoint from the local database; the Replicator requests the remote copy of the checkpoint, and the Checkpointer then compares them. If they don’t match, it throws them away and starts over.

When the Pusher and Puller update their state they tell the Checkpointer. The Replicator periodically saves updated checkpoints to the remote, and when that succeeds it tells the Checkpointer to update the local checkpoint.

Lower-level classes it uses are `Checkpoint` (its data model, basically), and `SequenceSet` (an efficient run-length-encoded set of sequence numbers.)

### Pusher

`Pusher` runs the push side of a replication:

1. Identifying all the revisions newer than the latest checkpoint
2. Sending the metadata of those revisions to the peer, which will respond with which ones it wants
3. Scheduling uploading the requested revisions to the peer
4. Uploading blobs to the peer on request
5. Updating the local sequence number in the checkpoint

It uses a `ChangesFeed` to tell it what revisions to send; that includes pre-existing changes, and, in a continuous replication, notifications of new changes as they happen.

It buffers this list of changes in a queue, and sends them over BLIP in batches of 200 using the `proposeChanges` (or `changes`, in a P2P replication) message.

The peer’s reply tells it which of those changes it actually wants. These go into another queue.

It takes changes from that queue, reads those revisions from the database, packages them up into `rev` messages, and sends them over BLIP.

It also has a BLIP handler that receives `getAttachment` messages that the peer sends when it finds a reference to a blob that it doesn’t have. In response it reads the blob from the local database and transmits it as the reply.

#### ChangesFeed

`ChangesFeed` uses both `C4DatabaseObserver` to get existing changes since the last checkpoint, and `C4DatabaseObserver` to get notifications of new changes. It filters these through the replicator’s push filter. It also looks up each document’s `remoteRevID` (the last known peer revision ID).

### Puller

The Puller’s job is:

1. Receiving lists of document/revision IDs from the peer and identifying which ones are not in the database
2. Receiving document revision bodies, and identifying which referenced blobs are not in the database
3. Receiving blobs and saving them
4. Saving the documents (in bulk)
5. Updating the remote sequence number in the checkpoint

Most of this work is done by other objects it owns, which are described below.

The Puller itself listens for `rev` messages containing the individual requested revisions. These are queued, and then handed off to members of a pool of `IncomingRev` objects that take over receiving the revision.

The Puller is notified when each IncomingRev instance completes its job; it then returns the instance to the pool, updates the Checkpointer, and updates its status.

#### RevFinder

`RevFinder` performs step 1 of the Puller’s job shown above. It has a BLIP handler that listens for `changes` messages from the peer, each containing a list of revision metadata (docID, revID, remote sequence.) It looks those up in the local database to see which don’t yet exist, and sends a reply asking for those.

#### IncomingRev

An `IncomingRev` instance handles a single document revision being pulled, performing steps 2 and 3. It receives the entire revision body, validates it, runs it past any client-supplied filter, and hands the `RevToInsert` object off to the `Inserter`.

If the revision body contains blob references, it looks up their digests in the local database; if a blob is not found, it sends a request back to the peer, receives the response, and adds the blob to the database. This has to complete before the revision itself can be added, to avoid dangling blob references.

#### Inserter

Inserter does step 4. It takes a bunch of completed `RevToInsert` objects and adds the revisions to the database using `C4Database::putDocument()`, all in one transaction. It then reports the success or failure of each revision back to its IncomingRev, which in turn notifies the Puller.

It uses a `Batcher` object to manage the incoming revisions, so it can handle many at a time — that’s important for performance, since a SQLite transaction has a lot of overhead. Typically the Inserter will spend most of its time inserting revisions and committing transactions, while meanwhile more RevToInsert objects are piling up in its batcher. Then when it’s done it processes the next batch.

### Minutiae

`ReplicatedRev`, and its subclasses `RevToSend` and `RevToInsert`, represent document revisions being processed by the replicator. At different stages they contain either just metadata, or the revision body as well.

The header `ReplicatorTuning.hh` contains a bunch of constants governing things like limits, queue sizes, and delays. Changing these will affect performance and memory usage … often in unintuitive ways. They shouldn’t be altered without doing profiling and measurements to make sure the change is helpful.

## BLIP

The BLIP protocol has its own [specification](../../Networking/BLIP/docs/BLIP%20Protocol.md).

The implementation in `Networking/BLIP/` has no dependencies on the higher level replicator, or on LiteCore’s API. It does use the WebSockets code (below) and a lot of LiteCore support classes, as well as Fleece.

The `litecore::blip::Connection` class manages a BLIP connection. It’s given a `WebSocket` instance that it uses for the actual communication. `Connection` is mostly a façade around an internal class, `BLIPIO`, that does most of the work.

BLIPIO is an Actor subclass, so outside calls are handled asynchronously and one at a time. It handles incoming WebSocket messages, unpacking each one into a BLIP frame that’s passed on to a `MessageIn` object. When a MessageIn receives its final frame, it’s dispatched to a handler (i.e. a replicator object.)

Requests to send messages are also handled asynchronously. BLIPIO keeps a queue of outgoing `MessageOut` objects and repeatedly pops one from the queue, sending up to 16KB of its data as a BLIP frame over the WebSocket. If the message has more data, it’s pushed back into the queue.

The sending loop is sensitive to back-pressure: if it’s writing data faster than it can be sent over the socket, or faster than the peer can process it, the WebSocket will tell it to pause until there’s room.


## WebSockets

The WebSocket code is pretty independent of most of LiteCore, using only low-level support classes and Fleece. (But `C4SocketImpl` does use the C4Socket API, for obvious reasons.)

### WebSocket

Abstract API of a WebSocket connection: it has pure-virtual methods for connecting / disconnecting, and sending / receiving messages.

### WebSocketImpl

Semi-abstract subclass of `WebSocket` that knows how to serialize and deserialize WebSocket messages, and manage specific messages like PING and CLOSE, but still doesn't provide actual network transport.

This class can operate in two different modes. In one mode it sends and receives raw bytes, and does its own WebSocket frame parsing and generation. In the other mode, it assumes someone else is doing the WebSocket parsing, so it just sends and receives the bodies of WebSocket messages.

### C4SocketImpl

A concrete subclass of `WebSocketImpl` that acts as glue between `WebSocketImpl` and the `C4SocketFactory` C API which provides the platform's actual network operations.

This class is used by most of the Couchbase Lite platforms, so that they can use their platform WebSocket or HTTP libraries to do the actual networking.

### BuiltInWebSocket

A batteries-included subclass of `WebSocketImpl` that does its own networking, using lower level classes like `TCPSocket`, `TLSContext`, and `HTTPLogic`. 

It supports POSIX and Windows socket APIs, thanks to a 3rd party library called `sockpp`. For TLS it uses `mbedTLS`.

This class is used by Couchbase Lite For C and by the `cblite` CLI tool.

### LoopbackWebSocket

An entirely different implementation of the `WebSocket` interface that simply passes the messages in memory between two instances of itself. 

This class is used for database-to-database replication, connecting two `Replicator` instances in the same process. It’s also used in replicator unit tests: see `ReplicatorLoopbackTest.cc`.
