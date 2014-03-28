//
//  CBForestDocument.h
//  CBForest
//
//  Created by Jens Alfke on 9/5/12.
//  Copyright (c) 2012 Couchbase, Inc. All rights reserved.
//

#import <Foundation/Foundation.h>
@class CBForestDB;


typedef UInt8 CBForestDocumentFlags;
enum {
    kCBForestDocDeleted    = 0x01,
    kCBForestDocConflicted = 0x02
};

extern const UInt64 kForestDocNoSequence;


/** Represents a single document in a CBForest.
    Instances are created by the CBForest object; never alloc/init one yourself. */
@interface CBForestDocument : NSObject

/** The CBForest that this document belongs to. */
@property (readonly) CBForestDB* db;

/** The document's ID. */
@property (readonly) NSString* docID;

/** The document's current sequence number in the database; this is a serial number that starts
    at 1 and is incremented every time any document is saved. */
@property (readonly) uint64_t sequence;

/** Is the document known to exist in the database?
    This will be YES for all documents other than those created by -makeDocumentWithID:. */
@property (readonly) BOOL exists;

/** Have the document's body, revID or flags been changed in memory since it was read or saved? */
@property (readonly) BOOL changed;

/** Length of the body (available even if the body hasn't been loaded) */
@property (readonly) UInt64 bodyLength;

/** Document body. May be nil if the document was explicitly loaded without its body. */
@property (copy) NSData* body;

/** Document revision ID metadata */
@property (copy) NSString* revID;

/** Document deleted/conflicted flag metadata */
@property CBForestDocumentFlags flags;

// I/O:

/** Refreshes the cached metadata (revID and flags) from the latest revision on disk. */
- (BOOL) refreshMeta: (NSError**)outError;

/** (Re-)reads the document's body and metadata from disk. */
- (NSData*) loadBody: (NSError**)outError;

/** Writes the document's current body and metadata to disk, if they've been changed. */
- (BOOL) saveChanges: (NSError**)outError;

@end
