<img src="logo.png">~

# The BLIP Protocol

**Version 3.0**

By [Jens Alfke](mailto:jens@mooseyard.com) (January 2019)

v3.0: Bumped WebSocket subprotocol string to "BLIP_v3".  
v3.0a2: Overhauled compression. Instead of separately compressing each message body (aside from properties), there’s now a separate compression context that’s used to sequentially compress/uncompress all frames with the Compressed flag. Also removed property tokenization.  
v2.0.1: No protocol changes; mostly just cleanup and extra details, especially about the WebSocket encoding. (It does add a confession that the "Bye" protocol for closing connections isn't actually implemented or honored by current implementations.)  
v2.0: Major update that bases the protocol on an underlying message transport (usually WebSockets), simplifies the header, and uses varint encodings.

## 1. Messages

The BLIP protocol runs over a bidirectional network connection and allows the peers on either end to send **messages** back and forth. The two types of messages are called **requests** and **responses**. Either peer may send a request at any time, to which the other peer will send back a response (unless the request has a special flag that indicates that it doesn't need a response.)

Messages are **multiplexed** in transit, so any number may be sent (or received) simultaneously. This differs from protocols like HTTP 1.x and WebSockets where a message blocks the stream until it's completely sent.

Messages have a structure similar to HTTP entities. Every message (request or response) has a **body**, and zero or more **properties**, or key-value pairs. The body is an uninterpreted sequence of bytes. Property keys and values must be UTF-8 strings. The only length constraint is that the encoded message-plus-properties cannot exceed 2^64-1 bytes.

Every message has a **request number**: Requests are numbered sequentially, starting from 1 when the connection opens. Each peer has its own independent sequence for numbering the requests that it sends. Each response is given the number of the corresponding request.

BLIP is typically layered atop the WebSocket protocol. The details of this are presented later.

### 1.1. Message Flags

Every request or response message has a set of flags that can be set by the application when it's created:

* **Compressed:** If this flag is set, the message data is compressed; see section 3.7 for details.
* **Urgent:** The implementation will attempt to expedite delivery of messages with this flag, by allocating them a greater share of the available bandwidth (but not to the extent of completely starving non-urgent messages.)
* **No-Reply**: This request does not need or expect a response. (This flag has no meaning in a response.)

### 1.2. Error Replies

A reply can indicate an error, at either the BLIP level (i.e. couldn't deliver the request to the recipient application) or the application level. In an error reply, the message properties provide structured information about the error:

* The "Error-Code" property's value is a decimal integer expressed in ASCII, in the range of a signed 32-bit integer.
* The "Error-Domain" property's value is a string denoting a domain in which the error code should be interpreted. If missing, its default value is "BLIP".
* Other properties may provide additional data about the error; applications can define their own schema for this.
* The message body, if non-empty, contains an error message in UTF-8 format.

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

> **Note:** The Objective-C implementation encodes Foundation framework NSErrors into error responses by storing the NSError's code and domain as the BLIP Error-Code and Error-Domain properties, and adding the contents of the NSError's userInfo dictionary as additional properties. When receiving an error response it decodes an NSError in the same way. This behavior is of course platform-specific, and is a convenience not mandated by the protocol.)

## 2. Message Delivery

Messages are **multiplexed** over the connection, so several may be in transit at once. This ensures that a long message doesn't block the delivery of others. It does mean that, on the receiving end, messages will not necessarily be _completed_ in order: if request number 3 is longer than average, then requests 4 and 6 might finish before it does and be delivered to the application first.

However, BLIP does guarantee that requests are _begun_ in order: the receiving peer will always get the first **frame** (chunk of bytes) of request number 3 before the first frame of any higher-numbered request.

The two peers can each send messages at whatever pace they wish. They don't have to take turns. Either peer can send a request whenever it wants.

Every incoming request must be responded to unless its "no-reply" flag is set. However, requests do not need to be responded to in order, and there's no built-in constraint on how long a peer can take to respond. If a peer has nothing meaningful to send back, but must respond, it can send back an empty response (with no properties and zero-length body.)

## 3. Protocol Details

### 3.1. The Transport

BLIP is layered atop a message-based protocol called the **transport**, typically [WebSocket][WEBSOCKET]. The details of this protocol are unimportant except that it must:

* Connect two peers (multicast is not supported)
* Deliver arbitrary-sized binary blobs (called **frames** in this document) in either direction
* Deliver these frames _reliably_ and _in order_

The transport connection between the two peers is opened in the normal way for the underlying protocol (i.e. WebSockets' HTTP-based handshake, optionally over SSL); BLIP doesn't specify how this happens.

There are currently no greetings or other preliminaries sent when the connection opens. Either peer, or both peers, just start sending messages when ready.

#### 3.1.1. WebSocket transport details

* A BLIP client opening a connection MUST request the WebSocket [subprotocol][SUBPROTOCOL] `BLIP_3`. A server supporting BLIP also MUST advertise support for this subprotocol. If one side supports BLIP but the other doesn't, BLIP messages cannot be sent; either side can close the connection or downgrade to some other WebSocket based schema.
* BLIP messages are sent in binary WebSocket messages; text messages are not used, and receiving one is a fatal connection error.
* Both BLIP and WebSocket use the terminology "messages" and "frames", where messages can be broken into sequences of frames. Try not to get them confused! A BLIP frame corresponds to (is sent as) a WebSocket message.

### 3.2. Sending Messages

Outgoing messages are multiplexed over the peer's transport, so that multiple large messages may be sent at once. Each message is encoded as binary data (including compression of the body, if desired) and that data is broken into a sequence of **frames**, typically either 16k or 4k bytes in size. The multiplexer then repeatedly chooses a message that's ready to send, and sends its next frame over the underlying transport (e.g. as a binary WebSocket message.) The algorithm works like this:

1. When the application submits a new message to be sent, the BLIP implementation assigns it a number: if it's a request it gets the next available request number, and if it's a response it gets the number of its corresponding request. It then puts the message into the out-box queue.
2. When the output stream is ready to send data, the BLIP implementation pops the first message from the head of the out-box and removes its next frame.
3. If the message has more frames remaining after this one, a **more-coming** flag is set in the frame's header, and the message is placed back into the out-box queue.
4. The frame is sent across the transport.

Normal messages are always placed into the queue at the tail end, which results in round-robin scheduling. Urgent messages follow a more complex rule:

* An urgent message is placed after the last other urgent message in the queue. 
* If there are one or more normal messages after that one, the message is inserted after the _first_ normal message (this prevents normal messages from being starved and never reaching the head of the queue.) Or if there are no urgent messages in the queue, the message is placed after the first normal message. If there are no messages at all, then there's only one place to put the message, of course.
* When a newly-ready urgent message is being added to the queue for the _first time_ (in step 1 above), it has the additional restriction that it must go _after_ any other message that has not yet had any of its frames sent. (This is so that messages are begun in sequential order; otherwise the first frame of urgent message number 10 might be sent before the first frame of regular message number 8, for example.)

### 3.3. Receiving Messages

The receiver simply reads the frames one at a time from the input transport and uses their message types and request numbers (sec. 3.5) to group them together into messages.

> **Note:** Requests (MSG) and responses (RPY) have _independent_ message number sequences, so a MSG frame with number 1 refers to a different message than an RPY frame with number 1. This is because RPY frames use the message numbering of the MSG they reply to. This is important to keep in mind when building a data structure that maps incoming message numbers to message objects!

When the current frame does not have its more-coming flag set, that message is complete. Its properties are decoded, its body is decompressed if necessary, and the message is delivered to the application.

### 3.4. Message Encoding

A message is encoded into binary data, prior to being broken into frames, as follows:

1. First the properties are encoded to binary, as alternating key and value strings. Each string is in C format: UTF-8 characters ending with a NUL byte. There is no padding.
2. Then the message is encoded:
  1. The encoded message begins with the length in bytes of the encoded properties, as an unsigned **[varint][VARINT]**. **Important Note:** If there are no properties, the length (zero) still needs to be written!
  2. After that come the encoded properties (if any).
  3. Then comes the message body. (It doesn't need a delimiter; it ends at the end of the final frame.)

### 3.5. Framing

Frames — chunks of messages — are what is actually sent to the transport. Each frame needs a header to identify it to the reader. The header consists of the _request number_ and the _frame flags_, each encoded as an unsigned [varint][VARINT].

> **Note:** The frame flags are currently always written as a single byte. The encoding is defined as a varint to leave room for future expansion. If a flag with value 0x80 or higher is ever defined, then flags may encode to two bytes.

The Request Number is the serial number of the request, as described above. (A reply frame uses the serial number of the request that it's a reply to.)

The Flags are as described in section 1.1, plus the more-coming flag described in 3.2. The encoding is defined as follows:

```
TypeMask  = 0x07  // 0000 0111
Compressed= 0x08  // 0000 1000
Urgent    = 0x10  // 0001 0000
NoReply   = 0x20  // 0010 0000
MoreComing= 0x40  // 0100 0000
```

The `TypeMask` is actually a 3-bit field, not a flag. Of the 8 possible message types, the ones currently defined are:

```
MSG =    0x00
RPY =    0x01
ERR =    0x02
ACKMSG = 0x04
ACKRPY = 0x05
```

The frame body data follows after the header, of course. If the Compressed flag is set, this data is compressed (sec. 3.6.)

> **Note:** Properties are encoded at the message level, not the frame level. That means that the first frame of a message -- but _only_ the first frame -- will have the properties' byte-count immediately following its header. In most cases the properties will appear only in the first frame, but if the encoded properties are too long to fit, the remainder might end up in subsequent frames.

Finally, all frame types, *except* `ACKMSG` and `ACKRPY`, end with a 4-byte checksum. This is a 32-bit integer in big-endian encoding (_not_ a varint). Its value is the running CRC32 checksum of all uncompressed frame body data, including the current frame's, transmitted thus far in this direction.

In summary, writing a frame goes like this:

1. Write the message number as an unsigned varint
2. Write the frame flags as an unsigned varint
3. Add the frame body to the output CRC32 checksum, unless this frame is an ACK
4. Compress the frame body, if the Compressed flag is set
5. Write the frame body
6. Write the CRC32 checksum as a 32-bit big-endian integer, unless this frame is an ACK

### 3.6. Compression

As of protocol version 3, compression is applied on a frame-by-frame basis. The client keeps a ‘deflate’ (RFC 1951) format compression context for outgoing frames, and an ‘inflate’ decompression context for incoming frames. The body of a frame with the Compress flag is compressed via the compression context before being sent. An incoming compressed frame's body is decompressed with the decompression context. Using the same context for successive frames improves the compression level (often greatly) when messages are short, since the compressor can leave out substrings shared with previous messages.

> **Note:** The 'Compress' flag is described as a message flag, but as of version 3 its effects are per-frame. It's legal, if unlikely, for a message to be sent with both compressed and uncompressed frames. The receiver SHOULD check each frame's flag instead of assuming that every frame will have the same compression status as the first.

The data being compressed is only the message data in the frame, not the header or checksum.

#### 3.6.1. Compression Algorithm

Data is compressed by the ubiquitous '[deflate][DEFLATE]' algorithm. This is the raw deflated data, _not_ wrapped in the 'gzip' or 'zlib' formats which add a header and a checksum. 

> **Note:** If you use the [zlib][ZLIB] library, make sure you initialize the contexts to use raw 'deflate' format, since its default format is 'zlib'.

The deflated data in a frame MUST consist of complete blocks, so that the receiver's decompression context can reproduce the original frame data without needing any further compressed bytes. This means that the compression context must be flushed after writing the frame.

Flushing a 'deflate' encoder always produces output that ends with the four-byte sequence `00 00 FF FF`. To save space, these bytes are removed from the frame. They must of course be added back before decoding a frame.

In summary, generating the body of a compressed frame works like this: 

1. Use the compression context to deflate enough message data (starting with the properties) to produce output of roughly the desired size.
2. Flush the context, to ensure that the data ends on a block boundary. (If using zlib, use the mode `Z_SYNC_FLUSH`.)
3. Remove the last four bytes of the deflated data (which must be `00 00 FF FF`).

The receiver of a compressed frame should do this:

1. Append the four bytes `00 00 FF FF` to the body of the frame.
2. Feed the result through the decompression context.
3. Flush the context to make sure it's written all of the inflated data to its output.

### 3.7. Flow Control

Flow control is necessary because different messages can be processed at different rates. A process might be receiving two large messages at once, and the frames of one message are processed more slowly (maybe they're being written to a file.) If the sender sends those frames too fast, the receiver will have to buffer them and its memory usage will keep going up. But the receiver can't just stop reading from the socket, or the other faster message receiver will stop getting data.

BLIP provides per-message flow control via ACK frames that acknowledge receipt of data from a message. There are two types, ACKMSG and ACKRPY, the only difference being whether they acknowledge a MSG or RPY frame. The content of an ACK frame is a varint representing the total number of payload bytes received of that message so far.

* A process receiving a multi-frame message (request or reply) should send an ACK frame every time the number of bytes received exceeds a multiple of some byte interval (currently 50000 bytes.)
* A process sending a multi-frame message should stop sending frames of that message whenever the number of unacknowledged bytes (bytes sent minus highest byte count received in an ACK) exceeds a threshold (which is currenly 128000 bytes.) A message suspended this way is removed from the normal queue (sec. 3.2) until an ACK with a sufficiently high byte count is received.

### 3.8. Protocol Error Handling

Many types of errors could be found in the incoming data while the receiver is parsing it. Some errors are fatal, and the peer should respond by immediately closing the connection. Other errors, called frame errors, can be handled by ignoring the frame and going on to the next.

Fatal errors are:

* Bad varint encoding -- the frame cuts off in the middle of a multi-byte varint
* Missing header value -- either no flags, or just an empty frame
* Receiving a frame type that isn't used for BLIP, e.g. a non-binary WebSocket message
* Invalid 'deflate'-format data in a compressed frame
* A frame checksum that doesn't match the current input CRC32 checksum.

Frame errors are:

* Unknown message type (neither request nor response)
* Request number refers to an already-completed request or response (i.e. a prior frame with this number had its "more-coming" flag set to false)
* A property string contains invalid UTF-8
* The property data's length field is longer than the message
* The property data, if non-empty, does not end with a NUL byte
* The property data contains an odd number of NUL bytes

Note that it is _not_ an error if:

* Undefined flag bits are set (except for the ones that encode the message type). These bits can be ignored.
* Property keys are not recognized by the application (BLIP itself doesn't care what the property keys mean. It's up to the application to decide what to do about such properties.)

[WEBSOCKET]: https://en.wikipedia.org/wiki/WebSocket
[SUBPROTOCOL]: https://hpbn.co/websocket/#subprotocol-negotiation
[VARINT]: (http://techoverflow.net/blog/2013/01/25/efficiently-encoding-variable-length-integers-in-cc/)
[DEFLATE]: https://tools.ietf.org/html/rfc1951
[ZLIB]: https://zlib.net
