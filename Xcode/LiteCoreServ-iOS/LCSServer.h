//
//  LCSServer.h
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

#import <Foundation/Foundation.h>
@class LCSServerConfig;

NS_ASSUME_NONNULL_BEGIN

@protocol LCSServerDelegate;

@interface LCSServer : NSObject

@property (nonatomic, weak) id <LCSServerDelegate> delegate;

@property (nonatomic, copy) LCSServerConfig* config;

@property (readonly, nonatomic) BOOL isListenerRunning;

@property (readonly, nonatomic) NSError* error;

+ (instancetype) sharedInstance;

- (instancetype) init NS_UNAVAILABLE;

- (BOOL) start;

- (BOOL) stop;

@end

@protocol LCSServerDelegate <NSObject>
@optional
- (void) didStartListenerWithError: (nullable NSError*)error;
- (void) didStopListener;
@end

NS_ASSUME_NONNULL_END
