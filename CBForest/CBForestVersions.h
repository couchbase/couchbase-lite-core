//
//  CBForestVersions.h
//  CBForest
//
//  Created by Jens Alfke on 12/3/12.
//  Copyright (c) 2012 Couchbase, Inc. All rights reserved.
//

#import "CBForest/CBForestDocument.h"


typedef UInt8 CBForestVersionsFlags;
enum {
    kCBForestDocDeleted    = 0x01,
    kCBForestDocConflicted = 0x02,
};


/** Version-tracking document.
    Uses the document's body as a structured store of a CouchDB-style revision tree.
    Don't use the inherited accessors for the metadata and body; instead use the API below
    to access revisions. */
@interface CBForestVersions : CBForestDocument

/** The current flags. (Stored in the metadata for efficient access.) */
@property (readonly) CBForestVersionsFlags flags;

/** The current revision ID. (Stored in the metadata for efficient access.) */
@property (readonly) NSString* revID;

/** Max depth the tree can grow to; older revisions will be pruned to enforce this. */
@property unsigned maxDepth;

/** Body of the current revision. */
@property (readonly) NSData* currentRevisionData;

/** Number of revisions stored. */
@property (readonly) NSUInteger revisionCount;

/** Is there a revision with the given ID? */
- (BOOL) hasRevision: (NSString*)revID;

/** Is there a revision with the given ID? */
- (BOOL) hasRevision: (NSString*)revID isLeaf: (BOOL*)outIsLeaf;

/** Returns the data of the revision with the given ID, or nil if it's not found. */
- (NSData*) dataOfRevision: (NSString*)revID;

/** Is the revision with the given ID a deletion (tombstone)?
    This is NOT the same thing as the ForestDB document itself being deleted. */
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
            parentID: (NSString*)parentRevID
       allowConflict: (BOOL)allowConflict;

/** Adds a revision to the tree, plus its ancestry.
    history[0] is the new revision's ID, history[1] is its parent, etc.
    Returns the index in history of the common ancestor, the 1st rev that already existed.,
    or -1 on error. */
- (NSInteger) addRevision: (NSData *)data
                 deletion: (BOOL)deletion
                  history: (NSArray*)history;

/** Saves changes made by -addRevision:. No-op if there haven't been any changes. */
- (BOOL) save: (NSError**)outError;

/** Returns a dump of info about all the revisions, for debugging. */
- (NSString*) dump;

@end
