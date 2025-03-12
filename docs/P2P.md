# Peer-To-Peer

March 4 2025

The component described here is the **Peer Discovery** API, the lowest level part. It's responsible for discovering other peers on the network and opening socket connections to them on demand, as well as making itself discoverable and handling incoming connections from other peers.

The higher-level component (TBD) will use this API to decide which peer(s) to connect to and replicate with.

> Warning: This API is settling down, but is still in flux and may change.


## Terminology

- A **Peer** is a view of another device running CBL.
- **Browsing** is the task of discovering other peers.
- **Publishing** is the task of making this app discoverable to other devices as a peer, and accepting incoming connections.
- **Metadata** is a set of key-value pairs that peers can expose, which other peers can read and be notified of changes to. Reading a peer's metadata is often cheaper than connecting directly to it.
- A **Provider** is a class that implements peer discovery and connections for some protocol, as a wrapper for a platform-specific API.
- A **Connection** is a bidirectional data stream to/from a peer, implemented as a `C4Socket`. Some protocols may use regular ol' WebSockets, but others will use custom `C4SocketFactory` implementations.


## The API Classes

The API is currently available only in C++, because in C form it would be awkward and a lot more complicated; the only platform that would require C (.NET) is not supported in this release.

The API consists of four classes in the header `c4PeerDiscovery.hh`:

- `C4PeerDiscovery`, which manages peer discovery and publishing.
- `C4PeerDiscovery::Observer`, an abstract interface that delivers notifications about changes in status, peers coming online or going offline, and incoming connections.
- `C4PeerDiscoveryProvider`, an abstract class that's subclassed by platform-specific code providing an implementation of peer discovery, usually for a single protocol. 
- `C4Peer`, which represents a single peer found by a provider.


## Thread-Safety

This entire API is thread-safe. Methods can be called on any thread without requiring locking. Conversely, callbacks can be issued from arbitrary threads and should not block. The implementation does not hold any locks while calling into platform code, so deadlocks should not be an issue.


## Using Peer Discovery

This will mostly be done by the higher-level peer-to-peer components in LiteCore.

1. Register the provider(s) by calling the static `C4PeerDiscovery::registerProvider`.
2. Instantiate `C4PeerDiscovery`, passing it a "serviceID" string that identifies the application's P2P service. The serviceID will be used to advertise the device and discover other peers.
3. Instantiate an observer and register it with `C4PeerDiscovery::addObserver`.
4. Call `C4PeerDiscovery::startBrowsing` and `C4PeerDiscovery::startPublishing`.
5. Track the set of peers by monitoring the `addedPeer` and `removedPeer` calls to your observer.
6. If you want to watch the metadata of peers, call their `monitorMetadata` methods.
7. You can update your own metadata by calling `C4PeerDiscovery::updateMetadata`.
8. Respond to an `incomingConnection` notification by creating an incoming replicator on the provided `C4Socket`.
9. To initiate replication with a peer:
   1. Call its `resolveURL` method, passing it a callback.
   2. When the callback is called, you're given a URL for the peer as well as a `C4SocketFactory`.
   3. Use those to start a replicator. (The socket factory goes in `C4ReplicatorParameters::socketFactory`.)
10. To stop peer discovery, call `C4PeerDiscovery::stopBrowsing` and `C4PeerDiscovery::stopPublishing`.

Note that you never need to call a `C4PeerDiscoveryProvider` directly. The only attribute you might want to use is its `name`, for logging purposes or to identify a specific provider (e.g. to prioritize DNS-SD over Bluetooth when connecting.)


## Implementing a Provider

To implement a provider for a protocol, you subclass `C4PeerDiscoveryProvider` and implement the abstract methods. These are all asynchronous, except for `getSocketFactory`. They should return quickly and announce results or changes by calling other methods, as described below. They must be thread-safe.

At runtime, create a singleton instance and call its `registerProvider` method so that `C4PeerDiscovery` knows about it.

`C4PeerDiscoveryProvider` isn't called directly by client code, only by `C4PeerDiscovery` and `C4Peer`.

### `startBrowsing`, `stopBrowsing`

Called by the `C4PeerDiscovery` methods of the same names.

Call `this->browseStateChanged` when browsing has started or stopped (including due to errors.)

Call `this->addPeer` when a peer is discovered, and `removePeer` if it goes offline. You can instantiate `C4Peer` directly, or create your own subclass.

### `monitorMetadata`

Called by `C4Peer::monitorMetadata`. The flag says whether to start or stop monitoring that peer.

Whenever you receive new metadata for a peer, call its `updateMetadata` method. (You don't need to check if it's different; the peer will ignore the call if it's not.)

### `resolveURL`, `cancelResolveURL`, `getSocketFactory`

`resolveURL` is called by `C4Peer::resolveURL`. It's a one-shot operation. Once you have a URL for the peer, call its `resolvedURL` method. Or if there's an error, call the same method but pass an empty URL and a `C4Error`.

If peer connections are made over regular TCP/IP WebSockets, use a regular `ws:` or `wss:` URL, and return `nullptr` from `getSocketFactory`, and you're done. Connections will be made by the platform's existing WebSocket implementation.

Otherwise, the details of the URL are up to you; at a minimum it needs a schema and a hostname so that C4Address can parse it. Use the hostname and/or port parts of the URL to store whatever addressing your C4SocketFactory needs; for example, Bluetooth uses the peer's device UUID as the "hostname" and the PSM number as the "port".

The `getSocketFactory` method should return a custom `C4SocketFactory` for the protocol you're implementing. In particular, its `open` function will be passed the URL you created, as parsed into a `C4Address`.

### `startPublishing`, `stopPublishing`, `updateMetadata`

Called by the `C4PeerDiscovery` methods of the same names, these are the flip-sides of the browsing methods above. `startPublishing` should make this device discoverable; `stopPublishing` should turn that off. You call `publishStateChanged` when the state changes.

`updateMetadata` should update the metadata you're publishing, such that other peers will discover the change. It doesn't need to call anything back.

#### Incoming connections

Unless peer connections are made over regular TCP/IP WebSockets, you'll need to listen for incoming connections while publishing. In response you create a `C4Socket` using your custom `C4SocketFactory` and call `this->notifyIncomingConnection`. If this method returns false, the connection was not handled by any observer, so you should close it yourself.

### `shutdown`

Called by `C4PeerDiscovery`'s destructor, prior to deleting your provider instance.

Your implementation should stop browsing and publishing, and do any other necessary asynchronous work with your underlying API. Then call the callback that was passed to `shutdown`.

Soon after that, your provider will be deleted.
