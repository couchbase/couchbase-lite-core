//
//  CouchbaseLiteReplicator.h
//  Replicator
//
//  Created by Jens Alfke on 2/13/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#import <Foundation/Foundation.h>

#import <CouchbaseLiteReplicator/c4Replicator.h>
#import <CouchbaseLiteReplicator/c4Socket.h>

#ifdef __OBJC__
#import <CouchbaseLiteReplicator/CBLWebSocket.h>
#endif

FOUNDATION_EXPORT double CouchbaseLiteReplicatorVersionNumber;
FOUNDATION_EXPORT const unsigned char CouchbaseLiteReplicatorVersionString[];
