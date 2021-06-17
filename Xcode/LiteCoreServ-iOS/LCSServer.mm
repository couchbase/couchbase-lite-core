//
//  LCSServer.mm
//  LiteCoreServ-iOS
//
//  Created by Pasin Suriyentrakorn on 6/1/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#import "LCSServer.h"
#import "LCSServerConfig.h"
#include "c4CppUtils.hh"
#include "c4Listener.h"
#include "Request.hh"
#include "Server.hh"

using namespace fleece;
using namespace litecore::REST;

@interface LCSServer ()
- (void) stopListener;
@end

@implementation LCSServer {
    C4Listener* _listener;
    Server* _adminServer;
    LCSServerConfig* _config;
    NSError* _error;
}

@synthesize delegate;


+ (instancetype)sharedInstance {
    static dispatch_once_t once;
    static id sharedInstance;
    dispatch_once(&once, ^{
        sharedInstance = [[self alloc] init];
    });
    return sharedInstance;
}


- /* private */(instancetype) init {
    self = [super init];
    if (self) {
        auto listenerLog = c4log_getDomain("Listener", true);
        c4log_setLevel(listenerLog, kC4LogInfo);
        
        _config = [[LCSServerConfig alloc] init];
    }
    return self;
}


- (BOOL) start {
    [self startAdminServer];
    [self startListener];
    return _error == nil;
}


- (BOOL) stop {
    [self stopListener];
    [self stopAdminServer];
    return YES;
}


- (void) dealloc {
    [self stop];
}


- (void) setConfig: (LCSServerConfig *)config {
    _config = [config copy];
    if (_adminServer) {
        [self stop];
        [self start];
    }
}


- (LCSServerConfig*) config {
    return [_config copy];
}


- (BOOL) isRunning {
    return _adminServer != nullptr;
}


- (BOOL) isListenerRunning {
    return _listener != nullptr;
}


- (NSError*) error {
    return _error;
}


#pragma mark - private


- (const char*) databaseDirectory {
    NSSearchPathDirectory dirID = NSApplicationSupportDirectory;
#if TARGET_OS_TV
    dirID = NSCachesDirectory; // Apple TV only allows apps to store data in the Caches directory
#endif
    NSArray* paths = NSSearchPathForDirectoriesInDomains(dirID, NSUserDomainMask, YES);
    NSString* path = paths[0];
    return [[path stringByAppendingPathComponent: @"CouchbaseLite"] UTF8String];
}


- (const char*) adminPort {
    return [[NSString stringWithFormat: @"%lu", (unsigned long)_config.adminPort] UTF8String];
}


- (void) startAdminServer {
    if (_adminServer)
        return;
    
    const char* options[] {
        "listening_ports",          [self adminPort],
        "enable_keep_alive",        "yes",
        "keep_alive_timeout_ms",    "1000",
        "num_threads",              "5",
        "decode_url",               "no",   // otherwise it decodes escaped slashes
        nullptr
    };
    
    _adminServer = new Server(options, (__bridge void*)self);
    _adminServer->addHandler(Server::GET, "/$", handleGETRoot);
    _adminServer->addHandler(Server::PUT, "/start$", handleStart);
    _adminServer->addHandler(Server::PUT, "/stop$", handleStop);
}


- (void) stopAdminServer {
    if (_adminServer != nullptr) {
        delete _adminServer;
        _adminServer = nullptr;
    }
}


- (void) startListener {
    if (_listener)
        return;
    
    C4ListenerConfig config = {};
    config.port = (uint16_t)_config.port;
    config.apis = kC4RESTAPI;
    config.allowCreateDBs = true;
    config.allowDeleteDBs = true;
    config.allowPush = true;
    config.allowPull = true;
    config.directory = c4str([self databaseDirectory]);
    
    C4Error c4Error;
    _listener = c4listener_start(&config, &c4Error);
    if (_listener == nullptr) {
        NSError* error;
        convertError(c4Error, &error);
        _error = error;
        NSLog(@"Cannot start the listener");
    } else
        _error = nil;
    
    id <LCSServerDelegate> d = self.delegate;
    if ([d respondsToSelector:@selector(didStartListenerWithError:)])
        [d didStartListenerWithError: _error];
}


- (void) stopListener {
    if (_listener != nullptr) {
        c4listener_free(_listener);
        _listener = nullptr;
        
        id <LCSServerDelegate> d = self.delegate;
        if ([d respondsToSelector:@selector(didStopListener)])
            [d didStopListener];
    }
}


#pragma mark - Admin handler


static void handleGETRoot(RequestResponse &rq) {
    LCSServer* serv = [LCSServer sharedInstance];
    LCSServerConfig* config = serv.config;
    auto &json = rq.jsonEncoder();
    json.beginDict();
    json.writeKey("adminPort"_sl);
    json.writeUInt(config.adminPort);
    
    if (serv.isListenerRunning) {
        json.writeKey("port"_sl);
        json.writeUInt(config.port);
    }
    
    if (serv.error) {
        json.writeKey("error"_sl);
        json.writeString([serv.error.description UTF8String]);
    }
    json.endDict();
}


static void handleStart(RequestResponse &rq) {
    Dict body = rq.bodyAsJSON().asDict();
    
    LCSServerConfig* c = [LCSServer sharedInstance].config;
    Value adminPort = body["adminPort"];
    if (adminPort)
        c.adminPort = (NSUInteger) adminPort.asInt();
    
    Value port = body["port"];
    if (port)
        c.port = (NSUInteger) port.asInt();
    
    dispatch_async(dispatch_get_main_queue(), ^{
        [LCSServer sharedInstance].config = c;
    });
    
    rq.respondWithStatus(HTTPStatus:: OK, "OK");
}


static void handleStop(RequestResponse &rq) {
    dispatch_async(dispatch_get_main_queue(), ^{
        [[LCSServer sharedInstance] stopListener];
    });
    
    rq.respondWithStatus(HTTPStatus:: OK, "OK");
}


#pragma mark - Error Conversion


static void convertError(const C4Error &c4err, NSError** outError) {
    NSCAssert(c4err.code != 0 && c4err.domain != 0, @"No C4Error");
    if (outError) {
        auto m = c4error_getMessage(c4err);
        NSString* desc = [[NSString alloc] initWithBytes: m.buf length: m.size
                                                encoding: NSUTF8StringEncoding];
        NSString* domain = [NSString stringWithFormat:@"LiteCoreServ/%d", c4err.domain];
        *outError = [NSError errorWithDomain: domain code: c4err.code
                                    userInfo: @{NSLocalizedDescriptionKey: desc}];
    }
}

@end
