//
//  LCSServerConfig.h
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

NS_ASSUME_NONNULL_BEGIN

@interface LCSServerConfig : NSObject <NSCopying>

@property (nonatomic) NSUInteger adminPort;

@property (nonatomic) NSUInteger port;

- (instancetype) init;

@end

NS_ASSUME_NONNULL_END
