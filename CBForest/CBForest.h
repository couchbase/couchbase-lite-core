//
//  CBForest.h
//  CBForest
//
//  Created by Jens Alfke on 9/4/12.
//  Copyright (c) 2012 Couchbase, Inc. All rights reserved.
//

#import <Foundation/Foundation.h>
@class CBForestDocument;


/** Option flag bits for opening a CBForest. */
typedef enum {
    kCBForestOpenCreate = 1,  /**< File will be created if it does not exist. */
    kCBForestOpenReadOnly = 2 /**< File will be opened read-only; saves will fail. */
} CBForestOpenOptions;

/** Option flag bigs for enumerating documents in a CBForest. */
typedef enum {
    kCBForestEnumerateMetaOnly
} CBForestEnumerationOptions;

/** NSError domain string for errors specific to CBForest. For error codes see error.h. */
extern NSString* const CBForestErrorDomain;

/** Callback block to pass to enumeration methods. */
typedef void (^CBForestIterator)(CBForestDocument* doc, BOOL *stop);


/** An open CBForest database. */
@interface CBForest : NSObject

/** Opens a database at the given filesystem path.
    @param filename The name of the file containing the database
    @param flags Additional flags for how the database should be opened. */
- (id) initWithFile: (NSString*)filePath
            options: (CBForestOpenOptions)options
              error: (NSError**)outError;

/** Closes the database. It's not strictly necessary to call this -- the database will be closed when this object is deallocated -- but it's a good way to ensure it gets closed in a timely manner.
    It's illegal to call any other of the methods defined here after closing the database; they will all raise exceptions. */
- (void) close;

/** The filesystem path the database was opened on. */
@property (readonly) NSString* filename;

/** Updates the database file header and makes sure all writes have been flushed to the disk.
    Until this happens, no changes made will persist: they aren't visible to any other client
    who opens the database, and will be lost if you close and re-open the database. */
- (BOOL) commit: (NSError**)outError;

/** Copies current versions of all documents to a new database at the given path.
    Afterwards, this database can be closed and deleted and the new one opened,
    without losing any currently-available data. */
- (BOOL) compactToFile: (NSString*)filePath
                 error: (NSError**)outError;

// DOCUMENTS:

/** Instantiates a CBForestDocument with the given document ID,
    but doesn't load its data or metadata yet.
    This method is used to create new documents; if you want to read an existing document
    it's better to call -documentWithID:error: instead. */
- (CBForestDocument*) makeDocumentWithID: (NSString*)docID;

/** Loads the metadata of the document with the given ID, into a CBForestDocument object. */
- (CBForestDocument*) documentWithID: (NSString*)docID
                               error: (NSError**)outError;

/** Loads the metadata of the document with the given sequence number,
    into a CBForestDocument object. */
- (CBForestDocument*) documentWithSequence: (uint64_t)sequence
                                     error: (NSError**)outError;

// ENUMERATING DOCUMENTS:

/** Iterates through all documents, in ascending order by key.
    @param startID  The document ID to start at, or nil to start from the beginning.
    @param options  kCBForestEnumerateOnlyDeletes and kCBForestEnumerateNoDeletes are supported.
    @param outError  On failure, an NSError will be stored here (unless it's NULL).
    @param block  The block to call for every document.
    @return  YES on success, NO on failure. */
- (BOOL) enumerateDocsFromID: (NSString*)startID
                        toID: (NSString*)endID
                     options: (CBForestEnumerationOptions)options
                       error: (NSError**)outError
                   withBlock: (CBForestIterator)block;

@end
