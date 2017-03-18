//
//  CBLWebSocket.m
//  StreamTaskTest
//
//  Created by Jens Alfke on 3/14/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#import "CBLWebSocket.h"
#import "CBLHTTPLogic.h"
#import "c4Socket.h"
#import "Logging.hh"
#import <CommonCrypto/CommonDigest.h>
#import <dispatch/dispatch.h>
#import <memory>


static constexpr size_t kMaxReceivedBytesPending = 100 * 1024;
static constexpr NSTimeInterval kConnectTimeout = 15.0;
static constexpr NSTimeInterval kIdleTimeout = 300.0;

static NSString* slice2string(C4Slice s) {
    if (!s.buf)
        return nil;
    return [[NSString alloc] initWithBytes: s.buf length: s.size
                                  encoding:NSUTF8StringEncoding];
}


extern void RegisterC4SocketFactory();
void RegisterC4SocketFactory() {
    [CBLWebSocket registerWithC4];
}


@interface CBLWebSocket ()
@property (readwrite, atomic) NSString* protocol;
@end


@implementation CBLWebSocket
{
    NSURLSession* _session;
    NSURLSessionStreamTask *_task;
    NSURLSessionDataTask* _httpTask;
    NSString* _expectedAcceptHeader;
    NSArray* _protocols;
    CBLHTTPLogic* _logic;
    C4Socket* _c4socket;
    NSTimer* _pingTimer;
    BOOL _receiving;
    size_t _receivedBytesPending, _sentBytesPending;
    CFAbsoluteTime _lastReadTime;
    NSOperationQueue* _queue;
}

@synthesize protocol=_protocol;


+ (void) registerWithC4 {
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        c4socket_registerFactory({
            .open = &doOpen,
            .close = &doClose,
            .write = &doWrite,
            .completedReceive = &doCompletedReceive
        });
        NSLog(@"Registered CBLWebSocket as C4SocketFactory");
    });
}

static void doOpen(C4Socket* s, const C4Address* addr) {
    NSURLComponents* c = [NSURLComponents new];
    c.scheme = slice2string(addr->scheme);
    c.host = slice2string(addr->hostname);
    c.port = @(addr->port);
    c.path = slice2string(addr->path);
    NSURL* url = c.URL;
    if (!url) {
        c4socket_closed(s, {LiteCoreDomain, kC4ErrorInvalidParameter});
        return;
    }
    auto socket = [[CBLWebSocket alloc] initWithURL: url c4socket: s];
    s->nativeHandle = (__bridge void*)socket;
    [socket start];
}

static void doClose(C4Socket* s) {
    [(__bridge CBLWebSocket*)s->nativeHandle closeSocket];
}

static void doWrite(C4Socket* s, C4SliceResult allocatedData) {
    [(__bridge CBLWebSocket*)s->nativeHandle writeAndFree: allocatedData];
}

static void doCompletedReceive(C4Socket* s, size_t byteCount) {
    [(__bridge CBLWebSocket*)s->nativeHandle completedReceive: byteCount];
}


- (instancetype) initWithURL: (NSURL*)url c4socket: (C4Socket*)c4socket {
    self = [super init];
    if (self) {
        _c4socket = c4socket;

        NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL: url];
        _logic = [[CBLHTTPLogic alloc] initWithURLRequest: request];
        _logic.handleRedirects = YES;

        _queue = [[NSOperationQueue alloc] init];
        _queue.name = @"WebSocket";
        auto disp = dispatch_queue_create("WebSocket", DISPATCH_QUEUE_SERIAL);
        _queue.underlyingQueue = disp;

        NSURLSessionConfiguration* conf = [NSURLSessionConfiguration defaultSessionConfiguration];
        _session = [NSURLSession sessionWithConfiguration: conf
                                                 delegate: self
                                            delegateQueue: _queue];
    }
    return self;
}


#pragma mark - HANDSHAKE:


- (void) start {
    [_queue addOperationWithBlock: ^{
        [self _start];
    }];
}


- (void) _start {
    NSLog(@"Connecting to %@:%hd...", _logic.URL.host, _logic.port);
    // Configure the nonce/key for the request:
    uint8_t nonceBytes[16];
    (void)SecRandomCopyBytes(kSecRandomDefault, sizeof(nonceBytes), nonceBytes);
    NSData* nonceData = [NSData dataWithBytes: nonceBytes length: sizeof(nonceBytes)];
    NSString* nonceKey = [nonceData base64EncodedStringWithOptions: 0];
    _expectedAcceptHeader = base64Digest([nonceKey stringByAppendingString:
                                          @"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"]);

    // Construct the HTTP request:
    _logic[@"Connection"] = @"Upgrade";
    _logic[@"Upgrade"] = @"websocket";
    _logic[@"Sec-WebSocket-Version"] = @"13";
    _logic[@"Sec-WebSocket-Key"] = nonceKey;
    if (_protocols)
        _logic[@"Sec-WebSocket-Protocol"] = [_protocols componentsJoinedByString: @","];

    _task = [_session streamTaskWithHostName: (NSString*)_logic.URL.host
                                        port: _logic.port];
    [_task resume];

    if (_logic.useTLS) {
        NSLog(@"Enabling TLS");
        [_task startSecureConnection];
    }

    [_task writeData: [_logic HTTPRequestData] timeout: kConnectTimeout
   completionHandler: ^(NSError* error) {
       NSLog(@"Sent HTTP request...");
       [self checkError: error];
       [self readHTTPResponse];
   }];
}


- (void) readHTTPResponse {
    CFHTTPMessageRef httpResponse = CFHTTPMessageCreateEmpty(NULL, false);
    [_task readDataOfMinLength: 1 maxLength: NSUIntegerMax timeout: kConnectTimeout
             completionHandler: ^(NSData* data, BOOL atEOF, NSError* error)
    {
        NSLog(@"(Received %zu bytes)", data.length);
        [self checkError: error];
        if (!CFHTTPMessageAppendBytes(httpResponse, (const UInt8*)data.bytes, data.length)) {
            // Error reading response!
            [self didCloseWithCode: kWebSocketCloseProtocolError
                            reason: @"Unparseable HTTP response"];
            return;
        }
        if (CFHTTPMessageIsHeaderComplete(httpResponse)) {
            [self receivedHTTPResponse: httpResponse];
            CFRelease(httpResponse);
        } else {
            [self readHTTPResponse];        // wait for more data
        }
    }];
}


- (void) receivedHTTPResponse: (CFHTTPMessageRef)httpResponse {
    [_logic receivedResponse: httpResponse];
    NSInteger httpStatus = _logic.httpStatus;

    if (_logic.shouldRetry) {
        // Retry the connection, due to a redirect or auth challenge:
        [_task cancel];
        _task = nil;
        [self start];

    } else if (httpStatus != 101) {
        // Unexpected HTTP status:
        C4WebSocketCloseCode closeCode = kWebSocketClosePolicyError;
        if (httpStatus >= 300 && httpStatus < 1000)
            closeCode = (C4WebSocketCloseCode)httpStatus;
        NSString* reason = CFBridgingRelease(CFHTTPMessageCopyResponseStatusLine(httpResponse));
        [self didCloseWithCode: closeCode reason: reason];

    } else if (!checkHeader(httpResponse, @"Connection", @"Upgrade", NO)) {
        [self didCloseWithCode: kWebSocketCloseProtocolError
                        reason: @"Invalid 'Connection' header"];
    } else if (!checkHeader(httpResponse, @"Upgrade", @"websocket", NO)) {
        [self didCloseWithCode: kWebSocketCloseProtocolError
                        reason: @"Invalid 'Upgrade' header"];
    } else if (!checkHeader(httpResponse, @"Sec-WebSocket-Accept", _expectedAcceptHeader, YES)) {
        [self didCloseWithCode: kWebSocketCloseProtocolError
                        reason: @"Invalid 'Sec-WebSocket-Accept' header"];
    } else {
        self.protocol = CFBridgingRelease(CFHTTPMessageCopyHeaderFieldValue(
                                                httpResponse, CFSTR("Sec-WebSocket-Protocol")));
        // TODO: Check Sec-WebSocket-Extensions for unknown extensions
        // Now I can start the WebSocket protocol:
        [self connected];
    }
}


- (void) connected {
    NSLog(@"CONNECTED!");
    _lastReadTime = CFAbsoluteTimeGetCurrent();
    [self receive];
    c4socket_opened(_c4socket);
}


#pragma mark - READ / WRITE:


// callback from C4Socket
- (void) writeAndFree: (C4SliceResult) allocatedData {
    NSData* data = [NSData dataWithBytesNoCopy: (void*)allocatedData.buf
                                        length: allocatedData.size
                                  freeWhenDone: NO];
    NSLog(@">>> sending %zu bytes: %@ ...", allocatedData.size, data);
    [_queue addOperationWithBlock: ^{
        [self->_task writeData: data timeout: kIdleTimeout
             completionHandler: ^(NSError* error) {
            [self checkError: error];
            NSLog(@"    (...sent bytes)");
            c4slice_free(allocatedData);
            c4socket_completedWrite(self->_c4socket, allocatedData.size);
        }];
    }];
}


- (void) receive {
    _receiving = true;
    [_task readDataOfMinLength: 1 maxLength: NSUIntegerMax timeout: kIdleTimeout
             completionHandler: ^(NSData* data, BOOL atEOF, NSError* error)
    {
        self->_receiving = false;
        self->_lastReadTime = CFAbsoluteTimeGetCurrent();
        if (error)
            [self didCloseWithError: error];
        else {
            self->_receivedBytesPending += data.length;
            NSLog(@"<<< received %zu bytes%s: %@", data.length, (atEOF ? " (EOF)" : ""), data);
            if (data) {
                c4socket_received(self->_c4socket, {data.bytes, data.length});
                if (!atEOF && self->_receivedBytesPending < kMaxReceivedBytesPending)
                    [self receive];
            }
        }
    }];
}


- (void) completedReceive: (size_t)byteCount {
    [_queue addOperationWithBlock: ^{
        self->_receivedBytesPending -= byteCount;
        if (!self->_receiving && self->_task != nil)
            [self receive];
    }];
}


// callback from C4Socket
- (void) closeSocket {
    [_queue addOperationWithBlock: ^{
        NSLog(@"closeSocket");
        [self->_task cancel];
        self->_task = nil;
    }];
}


#pragma mark - URL SESSION DELEGATE:


- (void)URLSession:(NSURLSession *)session readClosedForStreamTask:(NSURLSessionStreamTask *)streamTask
{
    NSLog(@"Read stream closed");
}


- (void)URLSession:(NSURLSession *)session writeClosedForStreamTask:(NSURLSessionStreamTask *)streamTask
{
    NSLog(@"Write stream closed");
}


- (void)URLSession:(NSURLSession *)session
              task:(NSURLSessionTask *)task
        didCompleteWithError:(nullable NSError *)error
{
    if (task == _task)
        _task = nil;
    [self checkError: error];
    NSLog(@"Completed");
}


#pragma mark - ERROR HANDLING:


- (void) checkError: (NSError*)error {
    if (error)
        [self didCloseWithError: error];
}


- (void) didCloseWithCode: (C4WebSocketCloseCode)code reason: (NSString*)reason {
    NSError* error = nil;
    if (code != kWebSocketCloseNormal) {
        error = [NSError errorWithDomain: @"WebSocket"
                                    code: code
                                userInfo: @{NSLocalizedFailureReasonErrorKey: reason}];
    }
    [self didCloseWithError: error];
}

- (void) didCloseWithError: (NSError*)error {
    C4Error c4err = {};
    if (error) {
        NSLog(@"CLOSED WITH ERROR: %@", error);
        //FIX: Better error mapping
        c4err.code = (int)error.code;
        NSString* domain = error.domain;
        if ([domain isEqualToString: @"WebSocket"])
            c4err.domain = WebSocketDomain;
        else if ([domain isEqualToString: NSPOSIXErrorDomain])
            c4err.domain = POSIXDomain;
        else {
            c4err = {LiteCoreDomain, -1};
        }
    } else {
        NSLog(@"CLOSED");
    }
    c4socket_closed(_c4socket, c4err);
}


#pragma mark - UTILITIES:


// Tests whether a header value matches the expected string.
static BOOL checkHeader(CFHTTPMessageRef msg, NSString* header, NSString* expected, BOOL caseSens) {
    NSString* value = CFBridgingRelease(CFHTTPMessageCopyHeaderFieldValue(msg,
                                                                  (__bridge CFStringRef)header));
    if (caseSens)
        return [value isEqualToString: expected];
    else
        return value && [value caseInsensitiveCompare: expected] == 0;
}


static NSString* base64Digest(NSString* string) {
    NSData* data = [string dataUsingEncoding: NSASCIIStringEncoding];
    unsigned char result[CC_SHA1_DIGEST_LENGTH];
    CC_SHA1([data bytes], (CC_LONG)[data length], result);
    data = [NSData dataWithBytes:result length:CC_SHA1_DIGEST_LENGTH];
    return [data base64EncodedStringWithOptions: 0];
}


#pragma mark - EXPERIMENTAL HANDSHAKE


#if 0
//NOTE: This doesn't work, at least not in macOS 10.12 / iOS 10.
- (void) startHTTP {
    // See if we can upgrade protocols (experimental)

    NSLog(@"Connecting to %@:%hd...", _logic.URL.host, _logic.port);
    // Configure the nonce/key for the request:
    uint8_t nonceBytes[16];
    (void)SecRandomCopyBytes(kSecRandomDefault, sizeof(nonceBytes), nonceBytes);
    NSData* nonceData = [NSData dataWithBytes: nonceBytes length: sizeof(nonceBytes)];
    NSString* nonceKey = [nonceData base64EncodedStringWithOptions: 0];
    _expectedAcceptHeader = base64Digest([nonceKey stringByAppendingString:
                                          @"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"]);

    NSMutableURLRequest* request = [NSMutableURLRequest requestWithURL: _logic.URL];
    request.HTTPShouldUsePipelining = YES;
    request.allHTTPHeaderFields = @{
                                    @"Connection": @"upgrade",      // <-- unfortunately CFNetwork changes this to 'keep-alive'
                                    @"Upgrade": @"websocket",
                                    @"Sec-WebSocket-Version": @"13",
                                    @"Sec-WebSocket-Key": nonceKey
                                    };
    _httpTask = [_session dataTaskWithRequest: request];
    [_httpTask resume];
}


- (BOOL) checkResponseStatus: (NSInteger)httpStatus headers: (NSDictionary*)headers {
    if (httpStatus != 101) {
        // Unexpected HTTP status:
        C4WebSocketCloseCode closeCode = kWebSocketClosePolicyError;
        if (httpStatus >= 300 && httpStatus < 1000)
            closeCode = (C4WebSocketCloseCode)httpStatus;
        //NSString* reason = CFBridgingRelease(CFHTTPMessageCopyResponseStatusLine(httpResponse));
        [self didCloseWithCode: closeCode reason: @"HTTP error"];
    } else if (!headerEquals(headers, @"Connection", @"Upgrade", NO)) {
        [self didCloseWithCode: kWebSocketCloseProtocolError
                        reason: @"Invalid 'Connection' header"];
    } else if (!headerEquals(headers, @"Upgrade", @"websocket", NO)) {
        [self didCloseWithCode: kWebSocketCloseProtocolError
                        reason: @"Invalid 'Upgrade' header"];
    } else if (!headerEquals(headers, @"Sec-WebSocket-Accept", _expectedAcceptHeader, YES)) {
        [self didCloseWithCode: kWebSocketCloseProtocolError
                        reason: @"Invalid 'Sec-WebSocket-Accept' header"];
    } else {
        // TODO: Check Sec-WebSocket-Extensions for unknown extensions
        return YES;
    }
    return NO;
}


static bool headerEquals(NSDictionary* headers, NSString* name, NSString* expected, BOOL caseSens) {
    NSString* value = headers[name];
    if (caseSens)
        return [value isEqualToString: expected];
    else
        return value && [value caseInsensitiveCompare: expected] == 0;
}


- (void)URLSession:(NSURLSession *)session
          dataTask:(NSURLSessionDataTask *)dataTask
didReceiveResponse:(NSURLResponse *)response
 completionHandler:(void (^)(NSURLSessionResponseDisposition disposition))completionHandler
{
    NSLog(@"Received response: %@", response);
    NSHTTPURLResponse* httpResponse = (NSHTTPURLResponse*)response;
    if ([self checkResponseStatus: httpResponse.statusCode
                          headers: httpResponse.allHeaderFields])
        completionHandler(NSURLSessionResponseBecomeStream);
    else
        completionHandler(NSURLSessionResponseCancel);
}


- (void)URLSession:(NSURLSession *)session
          dataTask:(NSURLSessionDataTask *)dataTask
didBecomeStreamTask:(NSURLSessionStreamTask *)streamTask
{
    NSLog(@"Upgraded to WebSocket!");
    _httpTask = nil;
    _task = streamTask;
    [self connected];
}
#endif


@end
