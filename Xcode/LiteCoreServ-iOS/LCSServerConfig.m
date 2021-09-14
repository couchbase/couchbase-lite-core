//
//  LCSServerConfig.m
//  LiteCoreServ-iOS
//
//  Created by Pasin Suriyentrakorn on 6/1/17.
//  Copyright 2017-Present Couchbase, Inc.
//
//  Use of this software is governed by the Business Source License included
//  in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
//  in that file, in accordance with the Business Source License, use of this
//  software will be governed by the Apache License, Version 2.0, included in
//  the file licenses/APL2.txt.
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
