//
// AppleBTSocketFactory.mm
//
// Copyright 2025-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#import "AppleBluetoothPeer+Internal.hh"
#import "Address.hh"
#import "c4ReplicatorTypes.h"
#import "c4Socket.h"
#import "Error.hh"
#import "Logging.hh"
#import "AppleBluetoothPeer.hh"
#import "StringUtil.hh"
#import "fleece/Fleece.hh"
#import "fleece/Expert.hh"
#import <CoreBluetooth/CoreBluetooth.h>
#import <dispatch/dispatch.h>
#import <memory>
#import <vector>

using namespace litecore;
using namespace fleece;


@interface LiteCoreBTSocket : NSObject <NSStreamDelegate>
- (instancetype) initWithPeerID: (slice)peerID
                       C4Socket: (C4Socket*)c4socket
                        options: (slice)options
                       provider: (C4PeerDiscoveryProvider*)provider;
- (instancetype) initWithPeerID: (slice) peerID
                incomingChannel: (CBL2CAPChannel*)channel;
- (void) connect;
- (void) redundantOpen;
- (void) closeSocket;
- (void) writeAndFree: (C4SliceResult) allocatedData;
- (void) completedReceive: (size_t)byteCount;
- (void) dispose;
@property (readonly, nonatomic) C4Socket* c4Socket;
@end


namespace litecore::p2p {

#define WSLog (*(LogDomain*)kC4WebSocketLog)

    // Number of bytes to read from the socket at a time
    static constexpr size_t kReadBufferSize = 32 * 1024;

    // Max number of bytes read that haven't been processed by LiteCore yet.
    // Beyond this point, I will stop reading from the socket, sending backpressure to the peer.
    static constexpr size_t kMaxReceivedBytesPending = 100 * 1024;

    struct PendingWrite {
        PendingWrite(NSData *d, void (^h)())
        :data(d)
        ,completionHandler(h)
        { }

        NSData *data;
        size_t bytesWritten {0};
        void (^completionHandler)();
    };

    static LiteCoreBTSocket* getWebSocket(C4Socket *s) {
        return (__bridge LiteCoreBTSocket*)c4Socket_getNativeHandle(s);
    }

    static void doOpen(C4Socket* c4sock, const C4Address* addr, C4Slice optionsFleece, void *context) {
        @autoreleasepool {
            if (auto btSocket = getWebSocket(c4sock)) {
                [btSocket redundantOpen];
            } else if (addr->scheme != kBTURLScheme) {
                c4socket_closed(c4sock, C4Error::make(LiteCoreDomain, kC4NetErrInvalidURL,
                                                      "Invalid URL scheme for Bluetooth"));
            } else {
                auto socket = [[LiteCoreBTSocket alloc] initWithPeerID: addr->hostname
                                                              C4Socket: c4sock
                                                               options: optionsFleece
                                                              provider: static_cast<C4PeerDiscoveryProvider*>(context)];
                [socket connect];
            }
        }
    }

    static void doClose(C4Socket* s) {
        [getWebSocket(s) closeSocket];
    }

    static void doWrite(C4Socket* s, C4SliceResult allocatedData) {
        [getWebSocket(s) writeAndFree: allocatedData];
    }

    static void doCompletedReceive(C4Socket* s, size_t byteCount) {
        [getWebSocket(s) completedReceive: byteCount];
    }

    static void doDispose(C4Socket* s) {
        [getWebSocket(s) dispose];
    }

    C4SocketFactory BTSocketFactory = {
        .framing = kC4WebSocketClientFraming,
        .open = &doOpen,
        .close = &doClose,
        .write = &doWrite,
        .completedReceive = &doCompletedReceive,
        .dispose = &doDispose,
    };

    Retained<C4Socket> BTSocketFromL2CAPChannel(CBL2CAPChannel* channel) {
        @autoreleasepool {
            auto btSocket = [[LiteCoreBTSocket alloc] initWithPeerID: channel.peer.identifier.UUIDString.UTF8String
                                                     incomingChannel: channel];
            return btSocket.c4Socket;
        }
    }

}

using namespace litecore::p2p;


@implementation LiteCoreBTSocket
{
    AllocedDict                 _options;
    dispatch_queue_t            _queue;
    C4Socket*                   _c4socket;
    bool                        _ownsSocket;
    C4PeerDiscoveryProvider*    _provider;
    id                          _keepMeAlive;

    std::string                 _peerID;
    CBL2CAPChannel*             _channel;
    NSInputStream*              _in;
    NSOutputStream*             _out;
    uint8_t*                    _readBuffer;
    std::vector<PendingWrite>   _pendingWrites;
    size_t                      _receivedBytesPending;
    bool                        _hasBytes;
    bool                        _hasSpace;
    bool                        _closing;
}

// initializer for outgoing connection that doesn't have a CBL2CAPChannel yet
- (instancetype) initWithPeerID: (slice)peerID
                       C4Socket: (C4Socket*)c4socket
                        options: (slice)options
                       provider: (C4PeerDiscoveryProvider*)provider
{
    self = [self initWithPeerID: peerID];
    if (self) {
        _options = AllocedDict(options);
        _c4socket = c4socket;
        c4Socket_setNativeHandle(_c4socket, (__bridge void*)self);
        _provider = provider;
        _keepMeAlive = self;          // Prevents dealloc until doDispose is called
    }
    return self;
}

// initializer for connection that already has a CBL2CAPChannel
- (instancetype) initWithPeerID: (slice)peerID
                incomingChannel: (CBL2CAPChannel*)channel
{
    self = [self initWithPeerID: peerID];
    if (self) {
        net::Address address(kBTURLScheme, peerID, channel.PSM, "/db");
        _c4socket = c4socket_fromNative2(BTSocketFactory, (__bridge void*)self, (C4Address*)address, true);
        _ownsSocket = true;
        _keepMeAlive = self;          // Prevents dealloc until doDispose is called
        [self setChannel: channel];
    }
    return self;
}

// common initializer
- (instancetype) initWithPeerID: (slice)peerID {
    self = [super init];
    if (self) {
        _peerID = peerID;
        std::string queueName = stringprintf("LiteCore-BTSocket-%.*s", FMTSLICE(peerID));
        _queue = dispatch_queue_create(queueName.c_str(), DISPATCH_QUEUE_SERIAL);
        _readBuffer = (uint8_t*)malloc(kReadBufferSize);
    }
    return self;
}

- (void) dealloc {
    LogToAt(WSLog, Verbose, "BTSocket %p: DEALLOC...", self);
    Assert(!_in, "Network stream was not closed");
    free(_readBuffer);
    if (_ownsSocket)
        c4socket_release(_c4socket);
}

- (void) dispose {
    LogToAt(WSLog, Verbose, "BTSocket %p: being disposed", self);

    // This has to be done synchronously, because _c4socket will be freed when this method returns
    [self callC4Socket: ^(C4Socket *socket) {
        // A lock is necessary as the socket could be accessed from another thread under the dispatch
        // queue, otherwise crash will happen as the c4socket will be freed after this.
        // The c4socket doesn't call dispose under a mutex so this is safe from being deadlock.
        c4Socket_setNativeHandle(socket, nullptr);
        self->_c4socket = nullptr;
    }];

    dispatch_async(_queue, ^{
        // CBSE-16151:
        //
        // The LiteCoreBTSocket may be called to dispose() by the c4socket before the
        // disconnect() can happen. For example, if the LiteCoreBTSocket cannot
        // call c4socket_closed() callback before the timeout (5 seconds),
        // the c4socket will call to dispose() the LiteCoreBTSocket right away.
        //
        // Therefore, before LiteCoreBTSocket is dealloc, we need to ensure that the
        // disconnect() is called to close the network steams and sockets. This
        // needs to be done under the same queue that the network streams and
        // c4socket's handlers/callbacks are using to avoid threading issues.
        //
        // Note: the LiteCoreBTSocket will be retained until this block is called
        // even though the _keepMeAlive is set to nil at the end of this
        // dispose method.
        if ([self isConnected]) {
            [self disconnect];
        }
    });
    
    // Remove the self-reference, so this object will be dealloced.
    self->_keepMeAlive = nil;
}

- (C4Socket*) c4Socket {return _c4socket;}

- (void) callC4Socket: (void (^)(C4Socket*))callback {
    @synchronized (self) {
        if (_c4socket) {
            callback(_c4socket);
        }
    }
}

#pragma mark - CONNECTING:

- (void) connect {
    Assert(!_channel);
    if (Retained<C4Peer> peer = _provider->discovery().peerWithID(_peerID)) {
        LogToAt(WSLog, Verbose, "BTSocket %p: connecting to %s", self, _peerID.c_str());
        Assert(peer->provider->name == "Bluetooth");
        OpenBTChannel(peer, ^(CBL2CAPChannel* channel, C4Error err) {
            dispatch_async(self->_queue, ^{
                if (channel) {
                    LogToAt(WSLog, Verbose, "BTSocket %p: connected", self);
                    [self setChannel: channel];
                    [self connected];
                } else {
                    [self c4SocketClosed: err];
                }
            });
        });
    } else {
        LogToAt(WSLog, Warning, "BTSocket %p: no known peer with ID %s", self, _peerID.c_str());
        [self c4SocketClosed: C4Error{NetworkDomain, kC4NetErrHostDown}];
    }
}

- (void) setChannel: (CBL2CAPChannel*)channel {
    Assert(!_channel);
    _channel = channel;
    _in = channel.inputStream;
    _out = channel.outputStream;

    CFReadStreamSetDispatchQueue((__bridge CFReadStreamRef)_in, _queue);
    CFWriteStreamSetDispatchQueue((__bridge CFWriteStreamRef)_out, _queue);
    _in.delegate = _out.delegate = self;
    
    [_in open];
    [_out open];
}

- (void) redundantOpen {
    dispatch_async(_queue, ^{
        [self connected];
    });
}

// Notifies LiteCore that the outgoing socket is connected.
- (void) connected {
    LogToAt(WSLog, Info, "BTSocket %p: CONNECTED!", self);
    [self callC4Socket:^(C4Socket *socket) {
        // Socket expects to receive HTTP response headers too:
        Encoder enc;
        enc.writeFormatted("{Connection: 'Upgrade', Upgrade: 'websocket', 'Sec-WebSocket-Protocol': 'BLIP_3+CBMobile_4'}");//FIXME: Don't hardcode this
        c4socket_gotHTTPResponse(socket, 101, enc.finish());

        c4socket_opened(socket);
    }];
}

#pragma mark - READ / WRITE:

// Returns true if there is too much unhandled WebSocket data in memory
// and we should stop reading from the socket.
- (bool) readThrottled {
    return _receivedBytesPending >= kMaxReceivedBytesPending;
}

// callback from C4Socket
- (void) writeAndFree: (C4SliceResult) allocatedData {
    NSData* data = [NSData dataWithBytesNoCopy: (void*)allocatedData.buf
                                        length: allocatedData.size
                                  freeWhenDone: NO];
    LogToAt(WSLog, Verbose, "BTSocket %p: >>> sending %zu bytes...", self, allocatedData.size);
    dispatch_async(_queue, ^{
        [self writeData: data completionHandler: ^() {
            size_t size = allocatedData.size;
            c4slice_free(allocatedData);
            LogToAt(WSLog, Verbose, "BTSocket %p:    (...sent %zu bytes)", self, size);
            [self callC4Socket:^(C4Socket *socket) {
                c4socket_completedWrite(socket, size);
            }];
        }];
    });
}

// Called when WebSocket data is received (NOT necessarily an entire message.)
- (void) receivedBytes: (const void*)bytes length: (size_t)length {
    self->_receivedBytesPending += length;
    LogToAt(WSLog, Verbose, "BTSocket %p: <<< received %zu bytes [now %zu pending]",
                  self, (size_t)length, self->_receivedBytesPending);
    [self callC4Socket:^(C4Socket *socket) {
        c4socket_received(socket, {bytes, length});
    }];
}

// callback from C4Socket
- (void) completedReceive: (size_t)byteCount {
    dispatch_async(_queue, ^{
        bool wasThrottled = self.readThrottled;
        self->_receivedBytesPending -= byteCount;
        if (self->_hasBytes && wasThrottled && !self.readThrottled)
            [self doRead];
    });
}

// Asynchronously sends data over the socket, and calls the completion handler block afterwards.
- (void) writeData: (NSData*)data completionHandler: (void (^)())completionHandler {
    _pendingWrites.emplace_back(data, completionHandler);
    if (_hasSpace)
        [self doWrite];
}

- (void) doWrite {
    while (!_pendingWrites.empty()) {
        auto &w = _pendingWrites.front();
        auto nBytes = [_out write: (const uint8_t*)w.data.bytes + w.bytesWritten
                        maxLength: w.data.length - w.bytesWritten];
        if (nBytes <= 0) {
            _hasSpace = false;
            return;
        }
        w.bytesWritten += nBytes;
        if (w.bytesWritten < w.data.length) {
            _hasSpace = false;
            return;
        }
        w.data = nil;
        if (w.completionHandler)
            w.completionHandler();
        _pendingWrites.erase(_pendingWrites.begin());
    }
}

- (void) doRead {
    LogToAt(WSLog, Verbose, "BTSocket %p: DoRead...", self);
    Assert(_hasBytes);
    _hasBytes = false;
    while (_in.hasBytesAvailable) {
        if (self.readThrottled) {
            _hasBytes = true;
            break;
        }
        NSInteger nBytes = [_in read: _readBuffer maxLength: kReadBufferSize];
        LogToAt(WSLog, Verbose, "BTSocket %p: DoRead read %zu bytes", self, nBytes);
        if (nBytes <= 0)
            break;
        [self receivedBytes: _readBuffer length: nBytes];
    }
}

- (void)stream: (NSStream*)stream handleEvent: (NSStreamEvent)eventCode {
    switch (eventCode) {
        case NSStreamEventOpenCompleted:
            LogToAt(WSLog, Verbose, "BTSocket %p: Open Completed on %s", self, stream.description.UTF8String);
            break;
        case NSStreamEventHasBytesAvailable:
            Assert(stream == _in);
            LogToAt(WSLog, Verbose, "BTSocket %p: HasBytesAvailable", self);
            _hasBytes = true;
            if (!self.readThrottled)
                [self doRead];
            break;
        case NSStreamEventHasSpaceAvailable:
            LogToAt(WSLog, Verbose, "BTSocket %p: HasSpaceAvailable", self);
            _hasSpace = true;
            [self doWrite];
            break;
        case NSStreamEventEndEncountered:
            LogToAt(WSLog, Verbose, "BTSocket %p: End Encountered on %s stream",
                          self, ((stream == _out) ? "write" : "read"));
            [self closeWithError: nil];
            break;
        case NSStreamEventErrorOccurred:
            LogToAt(WSLog, Verbose, "BTSocket %p: Error Encountered on %s", self, stream.description.UTF8String);
            [self closeWithError: stream.streamError];
            break;
        default:
            break;
    }
}

#pragma mark - CLOSING / ERROR HANDLING:

// callback from C4Socket
- (void) closeSocket {
    LogToAt(WSLog, Info, "BTSocket %p: closeSocket requested", self);
    dispatch_async(_queue, ^{
        if ([self isConnected]) {
            [self closeWithError: nil];
        }
    });
}

// Closes the connection and passes a WebSocket/HTTP status code to LiteCore.
- (void) closeWithCode: (C4WebSocketCloseCode)code reason: (NSString*)reason {
    if (code == kWebSocketCloseNormal) {
        [self closeWithError: nil];
        return;
    }
    if (!_in)
        return;
    
    LogToAt(WSLog, Info, "BTSocket %p: CLOSING WITH STATUS %d \"%s\"", self, (int)code, reason.UTF8String);
    [self disconnect];
    nsstring_slice reasonSlice(reason);
    [self c4SocketClosed: c4error_make(WebSocketDomain, code, reasonSlice)];
}

// Closes the connection and passes the NSError (if any) to LiteCore.
- (void) closeWithError: (NSError*)error {
    // This function is always called from queue.
    if (_closing) {
        LogToAt(WSLog, Verbose, "BTSocket %p: Websocket is already closing. Ignoring the close.", self);
        return;
    }
    _closing = YES;
    
    [self disconnect];
    
    C4Error c4err;
    if (error) {
        LogToAt(WSLog, Error, "BTSocket %p: CLOSED WITH ERROR: %s", self, error.description.UTF8String);
        //convertError(error, &c4err);
        c4err = C4Error::make(NetworkDomain, kC4NetErrUnknown); //TEMP
    } else {
        LogToAt(WSLog, Info, "BTSocket %p: CLOSED", self);
        c4err = {};
    }
    [self c4SocketClosed: c4err];
}

- (void) c4SocketClosed: (C4Error)c4err {
    [self callC4Socket:^(C4Socket *socket) {
        c4socket_closed(socket, c4err);
    }];
}

- (void) disconnect {
    LogToAt(WSLog, Verbose, "BTSocket %p: Disconnect", self);
    if (_in || _out) {
        _in.delegate = _out.delegate = nil;
        [_in close];
        [_out close];
        _in = nil;
        _out = nil;
    }
}

- (BOOL) isConnected {
    return (_in || _out);
}

@end
