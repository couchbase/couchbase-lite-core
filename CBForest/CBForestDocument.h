//
//  CBForestDocument.h
//  CBForest
//
//  Created by Jens Alfke on 9/5/12.
//  Copyright (c) 2012 Couchbase, Inc. All rights reserved.
//

#import <Foundation/Foundation.h>
@class CBForest;


/** Represents a single document in a CBForest.
    Instances are created by the CBForest object; never alloc/init one yourself. */
@interface CBForestDocument : NSObject

/** The CBForest that this document belongs to. */
@property (readonly) CBForest* db;

/** The document's ID. */
@property (readonly) NSString* docID;

/** The document's current sequence number in the database; this is a serial number that starts
    at 1 and is incremented every time any document is saved. */
@property (readonly) uint64_t sequence;

/** Is the document known to exist in the database?
    This will be YES for all documents other than those created by -makeDocumentWithID:. */
@property (readonly) BOOL exists;

/** Document body. */
@property (copy) NSData* data;

/** Document metadata. Clients can store anything they want here. */
@property (copy) NSData* metadata;

// I/O:

/** Refreshes the cached metadata from the latest revision on disk. */
- (BOOL) refreshMeta: (NSError**)outError;

/** Reads the document's data from disk. (The data is not cached in memory.) */
- (NSData*) loadData: (NSError**)outError;

/** Writes the document's current data and metadata to disk. */
- (BOOL) saveChanges: (NSError**)outError;

@end
