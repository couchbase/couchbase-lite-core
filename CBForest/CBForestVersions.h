//
//  CBForestVersions.h
//  CBForest
//
//  Created by Jens Alfke on 12/3/12.
//  Copyright (c) 2012 Couchbase, Inc. All rights reserved.
//

#import "CBForest/CBForestDocument.h"


/** Version-tracking layer on top of a CBForestDocument.
    Uses the document's body as a structured store of a CouchDB-style revision tree. */
@interface CBForestVersions : NSObject

/** Instantiates from a document. The document doesn't need to exist in the database yet. */
- (id) initWithDocument: (CBForestDocument*)doc
                  error: (NSError**)outError;

/** Max depth the tree can grow to; older revisions will be pruned to enforce this. */
@property unsigned maxDepth;

/** Body of the current revision. */
@property (readonly) NSData* currentRevisionData;

/** Number of revisions stored. */
@property (readonly) NSUInteger revisionCount;

/** Is there a revision with the given ID? */
- (BOOL) hasRevision: (NSString*)revID;

/** Returns the data of the revision with the given ID, or nil if it's not found. */
- (NSData*) dataOfRevision: (NSString*)revID;

/** Is the revision with the given ID a deletion (tombstone)? */
- (BOOL) isRevisionDeleted: (NSString*)revID;

/** Does the document have active conflicts? */
@property (readonly) BOOL hasConflicts;

/** The IDs of all non-deleted leaf revisions, i.e. conflicts.
    If there is no conflict, only the current revision's ID is returned. */
@property (readonly) NSArray* currentRevisionIDs;

/** Returns the IDs of the given revision and all its ancestors, in reverse order. */
- (NSArray*) historyOfRevision: (NSString*)revID;

/** Adds a revision to the tree. Returns YES on success, NO on error.
    Updates the document's .flags and .revisionID properties appropriately. */
- (BOOL) addRevision: (NSData*)data
            deletion: (BOOL)deletion
              withID: (NSString*)revID
            parentID: (NSString*)parentRevID;

/** Serializes the revision tree and saves it back to the document's body. */
- (BOOL) save: (NSError**)outError;

@end
