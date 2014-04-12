//
//  CBForestDocument.h
//  CBForest
//
//  Created by Jens Alfke on 9/5/12.
//  Copyright (c) 2012 Couchbase, Inc. All rights reserved.
//

#import "CBForestDB.h"


/** Represents a single document in a CBForest.
    Instances are created by the CBForest object; never alloc/init one yourself. */
@interface CBForestDocument : NSObject

/** The CBForest that this document belongs to. */
@property (readonly) CBForestDB* db;

/** The document's ID. */
@property (readonly) NSString* docID;

/** The document's metadata. */
@property (readonly) NSData* metadata;

/** The document's current sequence number in the database; this is a serial number that starts
    at 1 and is incremented every time any document is saved. */
@property (readonly) CBForestSequence sequence;

/** Is the document known to exist in the database? */
@property (readonly) BOOL exists;

/** Length of the body (available even if the body hasn't been loaded) */
@property (readonly) UInt64 bodyLength;

// I/O:

/** Reads the document's body from the database. */
- (NSData*) readBody: (NSError**)outError;

/** Refreshes the cached metadata from the latest revision on disk.
    If the document doesn't exist or has been deleted, no error is returned but the metadata
    is set to nil*/
- (BOOL) reloadMeta: (NSError**)outError;

/** Writes the document to the database. */
- (BOOL) writeBody: (NSData*)body
          metadata: (NSData*)metadata
             error: (NSError**)outError;

/** Deletes the document from the database. */
- (BOOL) deleteDocument: (NSError**)outError;

@end
