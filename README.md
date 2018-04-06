# BLIP-Cpp

![BLIP logo](docs/logo.png)

This is the C++ implementation of the BLIP network messaging protocol.
Unfortunately it's not turn-key yet; see the [Building/Using](#building-using) section below.

There are also implementations for [Go][BLIP_GO] and [Objective-C][BLIP_COCOA] (but the latter only supports the older protocol version.)

## "What's BLIP?"

You can think of BLIP as an extension of [WebSockets][WEBSOCKET]. (If you're not familiar with WebSockets, it's a simple protocol that runs over TCP and allows the peers to exchange _messages_ instead of raw bytes. It also uses an HTTP-based handshake to open the connection, which lets it interoperate smoothly with most middleware.)

BLIP adds a number of useful features that aren't supported by WebSockets:

* **Request/response:** Messages can have responses, and the responses don't have to be sent in the same order as the original messages. Responses are optional; a message can be sent in no-reply mode if it doesn't need one, otherwise a response (even an empty one) will always be sent after the message is handled.
* **Metadata:** Messages are structured, with a set of key/value headers and a binary body, much like HTTP or MIME messages. Peers can use the metadata to route incoming messages to different handlers, effectively creating multiple independent channels on the same connection.
* **Multiplexing:** Large messages are broken into fragments, and if multiple messages are ready to send their fragments will be interleaved on the connection, so they're sent in parallel. This prevents huge messages from blocking the connection.
* **Priorities:** Messages can be marked Urgent, which gives them higher priority in the multiplexing (but without completely starving normal-priority messages.) This is very useful for streaming media.

## Documentation

You can read the protocol documentation [here][BLIPDOCS].

API documentation for the C++ implementation isn't available yet, although many of the classes and methods have Doxygen-style header comments. The public headers are in the [include/blip_cpp/](include/blip_cpp) directory.

## Building / Using

The `master` branch is the latest, and implements the current version 3 protocol. If for some reason you need to use the older version 2, check out the `blip2` branch.

The Xcode project's `blip_cpp` target builds a static library. There's a CMake file too.

BLIP is dependent on a WebSocket implementation. This is abstracted as some interface-like classes in the [WebSocketInterfacel.hh](include/blip_cpp/WebSocketInterface.hh) header. There are two ways to provide a WebSocket implementation:

* Find an existing WebSocket implementation, and create a subclass of WebSocket that uses it.
* Subclass [WebSocketImpl](include/blip_cpp/WebSocketImpl.hh), which is an abstract subclass that implements message framing. You'll need to hook this up to a TCP socket, and provide code to run the HTTP handshake.

We would like to provide full implementations, but haven't had time yet to factor the code out of the [couchbase-lite-core repo](https://github.com/couchbase/couchbase-lite-core), which is currently the primary user of BLIP. Moreover, the choice of WebSocket implementation depends greatly on one's target platform(s) and version dependencies.

## History

BLIP has been around since 2008, when Jens Alfke designed it as a simpler alternative to BEEP and used it in an experimental unreleased P2P app. At that point it ran directly over a TCP socket with its own message framing.

In 2013 BLIP was redesigned and reimplemented as a layer atop WebSockets. This simplified the implementation and took advantage of the ability to tunnel WebSocket connections through HTTP proxies and other middleware.

From 2013 to 2015 BLIP was used experimentally as the substrate for a new replication protocol in Couchbase Mobile, but this didn't appear as a feature in a release. In the process, the protocol was updated slightly to resolve some issues with flow-control.

In 2017 the BLIP-based replication protocol is being used in Couchbase Lite 2.0 and Sync Gateway 1.5.

In January 2018 the protocol was updated to version 3, which is (unfortunately) incompatible with version 2. This implementation and the Go one have both been updated to version 3.


[WEBSOCKET]: http://www.websocket.org
[BLIPDOCS]: docs/BLIP%20Protocol.md
[BLIP_GO]: https://github.com/couchbase/go-blip
[BLIP_COCOA]: https://github.com/couchbaselabs/BLIP-Cocoa
