<img src="logo.png">

# The BLIP Protocol

**Version 2**

By [Jens Alfke](mailto:jens@mooseyard.com) (April 2015)

## 1. Messages

The BLIP protocol runs over a bidirectional network connection and allows the peers on either end to send **messages** back and forth. The two types of messages are called **requests** and **responses**. Either peer may send a request at any time, to which the other peer will send back a response (unless the request has a special flag that indicates that it doesn't need a response.)

Messages are **multiplexed** in transit, so any number may be sent (or received) simultaneously. This differs from protocols like HTTP 1.x and WebSockets where a message blocks the stream until it's completely sent.

Messages have a structure similar to HTTP entities. Every message (request or response) has a **body**, and zero or more **properties**, or key-value pairs. The body is an uninterpreted sequence of bytes. Property keys and values must be UTF-8 strings. The only length constraint is that the encoded message-plus-properties cannot exceed 2^64-1 bytes.

Every message has a **request number:** Requests are numbered sequentially, starting from 1 when the connection opens. Each peer has its own independent sequence for numbering the requests that it sends. Each response is given the number of the corresponding request.

Version 2 of BLIP is typically layered atop the WebSocket protocol. The details of this are presented later. (Version 1 included its own framing protocol and talked directly to a TCP socket.)

### 1.1. Message Flags

Every request or response message has a set of flags that can be set by the application when it's created:

* **Compressed:** If this flag is set, the body of the message (but not the property data!) will be compressed in transit via the gzip algorithm.
* **Urgent:** The implementation will attempt to expedite delivery of messages with this flag, by allocating them a greater share of the available bandwidth (but not to the extent of completely starving non-urgent messages.)
* **No-Reply**: This request does not need or expect a response. (This flag has no meaning in a response.)
* **Meta**: This flag indicates a message intended for internal use by the peer's BLIP implementation, and should not be delivered directly to the client application. (An example is the "bye" request used to negotiate closing the connection.)

### 1.2. Error Replies

A reply can indicate an error, at either the BLIP level (i.e. couldn't deliver the request to the recipient application) or the application level. In an error reply, the message properties provide information about the error:

* The "Error-Code" property's value is a decimal integer expressed in ASCII, in the range of a signed 32-bit integer.
* The "Error-Domain" property's value is a string denoting a domain in which the error code should be interpreted. If missing, its default value is "BLIP".
* Other properties may provide additional data about the error; applications can define their own schema for this.

The "BLIP" error domain uses the HTTP status codes, insofar as they make sense, as its error codes. The ones used thus far are:

```
BadRequest = 400,
Forbidden = 403,
NotFound = 404,
BadRange = 416,
HandlerFailed = 501,
Unspecified = 599 
```

Other error domains are application-specific, undefined by the protocol itself.

(**Note:** The Objective-C implementation encodes Foundation framework NSErrors into error responses by storing the NSError's code and domain as the BLIP Error-Code and Error-Domain properties, and adding the contents of the NSError's userInfo dictionary as additional properties. When receiving an error response it decodes an NSError in the same way. This behavior is of course platform-specific, and is a convenience not mandated by the protocol.)

## 2. Message Delivery

Messages are **multiplexed** over the connection, so several may be in transit at once. This ensures that a long message doesn't block the delivery of others. It does mean that, on the receiving end, messages will not necessarily be _completed_ in order: if request number 3 is longer than average, then requests 4 and 6 might finish before it does and be delivered to the application first.

However, BLIP does guarantee that requests are _begun_ in order: the receiving peer will always get the first **frame** (chunk of bytes) of request number 3 before the first frame of any higher-numbered request.

The two peers can each send messages at whatever pace they wish. They don't have to take turns. Either peer can send a request whenever it wants.

Every incoming request must be responded to unless its "no-reply" flag is set. However, requests do not need to be responded to in order, and there's no built-in constraint on how long a peer can take to respond. If a peer has nothing meaningful to send back, but must respond, it can send back an empty response (with no properties and zero-length body.)

## 3. Protocol Details

### 3.1. The Transport

BLIP 2 is layered atop a message-based protocol called the **transport**, typically WebSockets. The details of this protocol are unimportant except that it must:

* Connect two peers (multicast is not supported)
* Deliver arbitrary-sized binary blobs (called **frames** in this document) in either direction
* Deliver these frames _reliably_ and _in order_

The transport connection between the two peers is opened in the normal way for the underlying protocol (i.e. WebSockets' HTTP-based handshake, optionally over SSL); BLIP doesn't specify how this happens.

There are currently no greetings or other preliminaries sent when the connection opens. Either peer (or both peers) just start sending messages when ready. [A special greeting message may be defined in the future.]

### 3.2. Closing The Connection

To initiate closing the connection, the peer does the following:

1. It sends a special request, with the "meta" flag set and the "Profile" property set to the string "Bye".
2. It waits for a response. While waiting it must not send any further requests, although it must continue sending frames of requests that are already being sent, and must send responses to any incoming requests. 
3. Upon receiving a response to the "bye" request, if the response contains an error, the attempt to close has failed (most likely because the other peer refused) and the peer should return to the normal open state. If the close request was initiated by client code, it should notify the client of the failure.
4. Otherwise, if the response was successful, the peer must wait until all of its outgoing responses have been completely sent, and until all of its requests have received a complete response. It can then close the socket.

The protocol for receiving a close request is similar:

1. The peer decides whether to accept or refuse the request, probably by asking the client code. If it accepts, it sends back an empty response. If it refuses, it sends back an error response (403 Forbidden is the default error code) and remains in the regular "open" state.
2. After accepting the close, the peer goes into the same waiting state as in step 4 above. It must not send any new requests, although it must continue sending any partially-sent messages and must reply to responses; and it must wait until all responses are sent and all requests have been responded to before closing the socket.

Note that it's possible for both peers to decide to close the connection simultaneously, which means their "bye" requests will "cross in the mail". They should handle this gracefully. If a peer has sent a "bye" request and receives one from the other peer, it should respond affirmatively and continue waiting for its reply.

Note also that both peers are likely to close the socket at almost the same time, since each will be waiting for the final frames to be sent/received. This means that if a peer receives an EOF on the socket, it should check whether it's already ready to close the socket itself (i.e. it's exchanged "bye"s and has no pending frames to send or receive); if so, it should treat the EOF as a normal close, just as if it had closed the socket itself. (Otherwise, of course, the EOF is unexpected and should be treated as a fatal error.)

### 3.3. Sending Messages

Outgoing messages are multiplexed over the peer's transport, so that multiple large messages may be sent at once. Each message is encoded as binary data (including compression of the body, if desired) and that data is broken into a sequence of **frames**, typically either 16k or 4k bytes in size. The multiplexer then repeatedly chooses a message that's ready to send, and sends its next frame over the underlying transport (e.g. as a binary WebSocket message.) The algorithm works like this:

1. When the application submits a new message to be sent, the BLIP implementation assigns it a number: if it's a request it gets the next available request number, and if it's a response it gets the number of its corresponding request. It then puts the message into the out-box queue.
2. When the output stream is ready to send data, the BLIP implementation pops the first message from the head of the out-box and removes its next frame.
3. If the message has more frames remaining after this one, a **more-coming** flag is set in the frame's header, and the message is placed back into the out-box queue.
4. The frame is sent across the transport.

Normal messages are always placed into the queue at the tail end, which results in round-robin scheduling. Urgent messages follow a more complex rule:

* An urgent message is placed after the last other urgent message in the queue. 
* If there are one or more normal messages after that one, the message is inserted after the _first_ normal message (this prevents normal messages from being starved and never reaching the head of the queue.) Or if there are no urgent messages in the queue, the message is placed after the first normal message. If there are no messages at all, then there's only one place to put the message, of course.
* When a newly-ready urgent message is being added to the queue for the _first time_ (in step 1 above), it has the additional restriction that it must go _after_ any other message that has not yet had any of its frames sent. (This is so that messages are begun in sequential order; otherwise the first frame of urgent message number 10 might be sent before the first frame of regular message number 8, for example.)

### 3.4. Receiving Messages

The receiver simply reads the frames one at a time from the input transport and uses their message types and request numbers to group them together into messages. 

When the current frame does not have its more-coming flag set, that message is complete. Its properties are decoded, its body is decompressed if necessary, and the message is delivered to the application.

### 3.5. Message Encoding

A message is encoded into binary data, prior to being broken into frames, as follows:

1. The properties are written out in pairs as alternating key and value strings. Each string is in C format: UTF-8 characters ending with a NUL byte. There is no padding.
2. Certain common strings are abbreviated using a hardcoded dictionary. The abbreviations are strings consisting of a single control character (the ascii value of the character is the index of the string in the dictionary, starting at 1.) The current dictionary can be found in BLIPProperties.m in the reference implementation.
3. The total length in bytes of the encoded properties is prepended to the property data as an unsigned **[varint](http://techoverflow.net/blog/2013/01/25/efficiently-encoding-variable-length-integers-in-cc/)**. **Important Note:** If there are no properties, the length (zero) still needs to be written!
5. The body is appended to the property data.
4. If the message's "compressed" flag is set, the body is compressed using the gzip "deflate" algorithm.

### 3.6. Framing

Frames — chunks of messages — are what is actually sent to the transport. Each frame needs a header to identify it to the reader. The header consists of the _request number_ and the _frame flags_, each encoded as an unsigned **[varint](http://techoverflow.net/blog/2013/01/25/efficiently-encoding-variable-length-integers-in-cc/)**

The Request Number is the serial number of the request, as described above. (A reply frame uses the serial number of the request that it's a reply to.)

The Flags are as described in section 1.1, plus the more-coming flag described in 3.3. The encoding is defined as follows:
```
TypeMask  = 0x07
Compressed= 0x04
Urgent    = 0x08
NoReply   = 0x10
MoreComing= 0x20
Meta      = 0x40
```

The `TypeMask` is actually a 3-bit integer, not a flag. Of the 8 possible message types, the ones currently defined are:
```
MSG =    0x00
RPY =    0x01
ERR =    0x02
ACKMSG = 0x04
ACKRPY = 0x05
```

The frame data follows after the header, of course.

Note that properties are encoded at the message level, not the frame level. That means that the first frame of a message -- but _only_ the first frame -- will have the properties' byte-count immediately following its header. In most cases the properties will appear only in the first frame, but if the encoded properties are too long to fit, the remainder might end up in subsequent frames.

### 3.7. Flow Control

Flow control is necessary because different messages can be processed at different rates. A process might be receiving two messages at once, and the frames of one message are processed more slowly (maybe they're being written to a file.) If the sender sends those frames too fast, the receiver will have to buffer them and its memory usage will keep going up. But the receiver can't just stop reading from the socket, or the other faster message receiver will stop getting data.

BLIP provides per-message flow control via ACK frames that acknowledge receipt of data from a message. There are two types, ACKMSG and ACKRPY, the only difference being whether they acknowledge a MSG or RPY frame. The content of an ACK frame is a varint representing the total number of payload bytes received of that message so far.

* A process receiving a multi-frame message (request or reply) should send an ACK frame every time the number of bytes received exceeds a multiple of some byte interval (currently 50000 bytes.)
* A process sending a multi-frame message should stop sending frames of that message whenever the number of unacknowledged bytes (bytes sent minus highest byte count received in an ACK) exceeds a threshold (which is currenly 128000 bytes.)

### 3.8. Protocol Error Handling

Many types of errors could be found in the incoming data while the receiver is parsing it. Some errors are fatal, and the peer should respond by immediately closing the connection. Other errors, called frame errors, can be handled by ignoring the frame and going on to the next.

Fatal errors are:

* Bad varint encoding -- the frame cuts off in the middle of a multi-byte varint
* Missing header value -- either no flags, or just an empty frame
* Receiving a frame type that isn't used for BLIP, e.g. a non-binary WebSocket message

Frame errors are:

* Unknown message type (neither request nor response)
* Request number refers to an already-completed request or response (i.e. a prior frame with this number had its "more-coming" flag set to false)
* A property string contains invalid UTF-8
* The property data's length field is longer than the remaining frame data
* The property data, if non-empty, does not end with a NUL byte
* The body of a compressed frame fails to decompress

Note that it is _not_ an error if:

* Undefined flag bits are set (except for the ones that encode the message type). These bits can be ignored.
* Property keys are not recognized by the application (BLIP itself doesn't care what the property keys mean. It's up to the application to decide what to do about such properties.)
