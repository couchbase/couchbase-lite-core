//
//  CBForestVersions.h
//  CBForest
//
//  Created by Jens Alfke on 12/3/12.
//  Copyright (c) 2012 Couchbase, Inc. All rights reserved.
//

#import "CBForest/CBForestDocument.h"


@interface CBForestVersions : NSObject

- (id) initWithDocument: (CBForestDocument*)doc error: (NSError**)outError;

@property (readonly) NSString* currentRevisionID;
@property (readonly) NSData* currentRevisionData;
@property (readonly) BOOL currentRevisionDeleted;

@property (readonly) NSUInteger revisionCount;

- (BOOL) hasRevision: (NSString*)revID;
- (NSData*) dataOfRevision: (NSString*)revID;
- (BOOL) isRevisionDeleted: (NSString*)revID;

- (BOOL) hasConflicts;
- (NSArray*) currentRevisionIDs;

- (NSArray*) historyOfRevision: (NSString*)revID;

- (BOOL) addRevision: (NSData*)data withID: (NSString*)revID parent: (NSString*)parentRevID;

- (BOOL) save: (NSError**)outError;

@end
