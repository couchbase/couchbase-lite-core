//
//  LCSServerConfig.m
//  LiteCoreServ-iOS
//
//  Created by Pasin Suriyentrakorn on 6/1/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#import "LCSServerConfig.h"

#define kDefaultAdminPort 59850
#define kDefaultPort 59840

@implementation LCSServerConfig

@synthesize adminPort, port;


- (instancetype) init {
    self = [super init];
    if (self) {
        [self loadEnvConfig];
    }
    return self;
}


- (instancetype) copyWithZone:(NSZone *)zone {
    LCSServerConfig* c = [[self.class alloc] init];
    c.adminPort = self.adminPort;
    c.port = self.port;
    return c;
}


- (void) loadEnvConfig {
    NSDictionary* env = NSProcessInfo.processInfo.environment;
    
    if (env[@"adminPort"])
        self.adminPort = [env[@"adminPort"] unsignedIntegerValue];
    else
        self.adminPort = kDefaultAdminPort;
    
    if (env[@"port"])
        self.port = [env[@"port"] unsignedIntegerValue];
    else
        self.port = kDefaultPort;
}


@end
