//
//  LCSServerConfig.h
//  LiteCoreServ-iOS
//
//  Created by Pasin Suriyentrakorn on 6/1/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface LCSServerConfig : NSObject <NSCopying>

@property (nonatomic) NSUInteger adminPort;

@property (nonatomic) NSUInteger port;

- (instancetype) init;

@end

NS_ASSUME_NONNULL_END
