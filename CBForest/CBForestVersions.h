//
//  CBForestVersions.h
//  CBForest
//
//  Created by Jens Alfke on 12/3/12.
//  Copyright (c) 2012 Couchbase, Inc. All rights reserved.
//

#import "CBForest/CBForestDocument.h"


@interface CBForestVersions : NSObject

- (id) initWithDocument: (CBForestDocument*)doc
                  error: (NSError**)outError;

@property (readonly) NSData* currentRevisionData;

@property (readonly) NSUInteger revisionCount;

- (BOOL) hasRevision: (NSString*)revID;
- (NSData*) dataOfRevision: (NSString*)revID;
- (BOOL) isRevisionDeleted: (NSString*)revID;

@property (readonly) BOOL hasConflicts;
- (NSArray*) currentRevisionIDs;

- (NSArray*) historyOfRevision: (NSString*)revID;

- (BOOL) addRevision: (NSData*)data
            deletion: (BOOL)deletion
              withID: (NSString*)revID
            parentID: (NSString*)parentRevID;

- (BOOL) save: (NSError**)outError;

@end
