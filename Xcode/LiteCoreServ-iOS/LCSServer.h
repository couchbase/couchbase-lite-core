//
//  LCSServer.h
//  LiteCoreServ-iOS
//
//  Created by Pasin Suriyentrakorn on 6/1/17.
//  Copyright © 2017 Couchbase. All rights reserved.
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
