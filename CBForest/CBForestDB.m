//
//  CBForest.m
//  CBForest
//
//  Created by Jens Alfke on 9/4/12.
//  Copyright (c) 2012 Couchbase, Inc. All rights reserved.
//

#import "CBForestDB.h"
#import "CBForestPrivate.h"
#import "slice.h"


// Macro to run a ForestDB call returning a fdb_status on the _queue.
#define ONQUEUE(FNCALL) \
    ({ __block fdb_status _status; \
       dispatch_sync(_queue, ^{ _status = (FNCALL); }); \
       _status; })

// Macro to run a ForestDB call returning a fdb_status on the _queue, and convert its return code
// to a BOOL, storing any error into ERROR.
#define CHECK_ONQUEUE(FNCALL,ERROR) \
    Check(ONQUEUE(FNCALL), (ERROR))


NSString* const CBForestErrorDomain = @"CBForest";


@implementation CBForestDB
{
    fdb_handle *_db;
    fdb_open_flags _openFlags;
    int _transactionLevel;
    dispatch_queue_t _queue;
    CBForestToken* _writeLock;
}

@synthesize documentClass=_documentClass;


- (id) initWithFile: (NSString*)filePath
            options: (CBForestFileOptions)options
              error: (NSError**)outError
{
    self = [super init];
    if (self) {
        NSString* filename = filePath.lastPathComponent;
        NSString* dirname = filePath.stringByDeletingLastPathComponent.lastPathComponent;
        NSString* label = [NSString stringWithFormat: @"CBForestDB %@/%@", dirname, filename];
        _queue = dispatch_queue_create(label.UTF8String, DISPATCH_QUEUE_SERIAL);
        _writeLock = [[self class] lockForFile: filePath];
        _documentClass = [CBForestDocument class];
        assert(kCBForestDBCreate == FDB_OPEN_FLAG_CREATE); // sanity check
        assert(kCBForestDBReadOnly == FDB_OPEN_FLAG_RDONLY);
        _openFlags = (fdb_open_flags)options;
        if (![self open: filePath options: options error: outError])
            return nil;
    }
    return self;
}

- (void) dealloc {
    if (_queue) {
        dispatch_sync(_queue, ^{
            fdb_close(_db);
        });
    }
}

// Vends a recursive lock for any filesystem path. This ensures that two CBForestDB instances on
// the same file have the same lock, so they can get exclusive write access.
+ (CBForestToken*) lockForFile: (NSString*)path {
    path = path.stringByResolvingSymlinksInPath.stringByStandardizingPath;
    NSMapTable* sCache;
    @synchronized(self) {
        if (!sCache)
            sCache = [NSMapTable strongToWeakObjectsMapTable];
        CBForestToken* token = [sCache objectForKey: path];
        if (!token) {
            token = [[CBForestToken alloc] init];
            token.name = path;
            [sCache setObject: token forKey: path];
        }
        return token;
    }
}

- (BOOL) open: (NSString*)filePath options: (CBForestFileOptions)options error: (NSError**)outError
{
    NSAssert(!_db, @"Already open");
    __block fdb_status status;
    dispatch_sync(_queue, ^{
        // ForestDB doesn't yet pay any attention to the FDB_OPEN_FLAG_CREATE flag. --4/2014
        if ((options & FDB_OPEN_FLAG_CREATE)
                || [[NSFileManager defaultManager] fileExistsAtPath: filePath])
            status = fdb_open(&_db, filePath.fileSystemRepresentation, options, NULL);
        else
            status = FDB_RESULT_NO_SUCH_FILE;
    });
    return Check(status, outError);
}

// Only call this on the _queue
- (fdb_handle*) handle {
    NSAssert(_db, @"Already closed!");
    return _db;
}

- (void) close {
    dispatch_sync(_queue, ^{
        fdb_close(_db);
        _db = nil;
    });
}

- (NSString*) description {
    return [NSString stringWithFormat: @"%@[%@]", [self class],
            (_db ? self.filename : @"closed")];
}

- (NSString*) filename {
    __block fdb_info info;
    dispatch_sync(_queue, ^{
        fdb_get_dbinfo(self.handle, &info);
    });
    return [[NSFileManager defaultManager] stringWithFileSystemRepresentation: info.filename
                                                                    length: strlen(info.filename)];
}

- (CBForestDBInfo) info {
    __block fdb_info info;
    dispatch_sync(_queue, ^{
        fdb_get_dbinfo(self.handle, &info);
    });
    return (CBForestDBInfo) {
        //.headerRevNum = _db.cur_header_revnum,
        .dataSize       = info.space_used,
        .fileSize       = info.file_size,
        .documentCount  = info.doc_count,
        .lastSequence   = info.last_seqnum,
    };
}

- (BOOL) commit: (NSError**)outError {
    NSAssert(!self.isInTransaction, @"-commit can only be used inside a transaction");
//  NSLog(@"~~~~~ COMMIT ~~~~~");
    if (_openFlags & FDB_OPEN_FLAG_RDONLY)
        return YES; // no-op if read-only
    return CHECK_ONQUEUE(fdb_commit(_db, FDB_COMMIT_NORMAL), outError);
}


- (BOOL) deleteDatabase: (NSError**)outError {
    if (_openFlags & FDB_OPEN_FLAG_RDONLY)
        return Check(FDB_RESULT_RONLY_VIOLATION, outError);
    return [self inTransaction: ^BOOL {
        NSString* path = self.filename;
        [self close];
        return [[NSFileManager defaultManager] removeItemAtPath: path error: outError];
    }];
}


- (BOOL) erase: (NSError**)outError {
    return [self inTransaction: ^BOOL {
        NSString* path = self.filename;
        return [self deleteDatabase: outError]
            && [self open: path options: (_openFlags | kCBForestDBCreate) error: outError];
    }];
}

            
- (BOOL) compact: (NSError**)outError
{
    if (_openFlags & FDB_OPEN_FLAG_RDONLY)
        return Check(FDB_RESULT_RONLY_VIOLATION, outError);
    return [self inTransaction: ^BOOL {
        NSString* filename = self.filename;
        NSString* tempFile = [filename stringByAppendingPathExtension: @"cpt"];
        [[NSFileManager defaultManager] removeItemAtPath: tempFile error: NULL];

        if (!CHECK_ONQUEUE(fdb_compact(self.handle, tempFile.fileSystemRepresentation), outError)) {
            [[NSFileManager defaultManager] removeItemAtPath: tempFile error: NULL];
            return NO;
        }

        // Now replace the original file with the compacted one and re-open it:
        [self close];
        if (rename(tempFile.fileSystemRepresentation, filename.fileSystemRepresentation) < 0) {
            if (outError)
                *outError = [NSError errorWithDomain: NSPOSIXErrorDomain code: errno userInfo: nil];
            [[NSFileManager defaultManager] removeItemAtPath: tempFile error: NULL];
            [self open: filename options: _openFlags error: NULL];
            return NO;
        }
        return [self open: filename options: _openFlags error: outError];
    }];
}


#pragma mark - TRANSACTIONS:


- (BOOL) isInTransaction {
    __block BOOL result;
    dispatch_sync(_queue, ^{
        result = (_transactionLevel > 0);
    });
    return result;
}


- (void) beginTransaction {
    dispatch_async(_queue, ^{
        if (_transactionLevel++ == 0)
            [_writeLock lockWithOwner: self];
    });
}


- (BOOL) endTransaction: (NSError**)outError {
    __block BOOL result = YES;
    dispatch_sync(_queue, ^{
        NSAssert(_transactionLevel > 0, @"CBForestDB: endTransaction without beginTransaction");
        if (--_transactionLevel == 0) {
            // Ending the outermost transaction:
            if (_db)
                result = Check(fdb_commit(_db, FDB_COMMIT_NORMAL), outError);
            [_writeLock unlockWithOwner: self];
        }
    });
    return result;
}


- (BOOL) inTransaction: (BOOL(^)())block {
    BOOL ok;
    [self beginTransaction];
    @try {
        ok = block();
    } @catch (NSException* x) {
        // MYReportException(x, @"CBForestDB transaction");
        NSLog(@"WARNING: Exception in CBForestDB transaction: %@", x);
        ok = NO;
    } @finally {
        ok = [self endTransaction: NULL] && ok;
    }
    return ok;
}


#pragma mark - KEYS/VALUES:


- (CBForestSequence) setValue: (NSData*)value meta: (NSData*)meta forKey: (NSData*)key
                        error: (NSError**)outError
{
    fdb_doc doc = {
        .key = (void*)key.bytes,
        .keylen = key.length,
        .body = (void*)value.bytes,
        .bodylen = value.length,
        .meta = (void*)meta.bytes,
        .metalen = meta.length
    };
    return [self rawSet: &doc error: outError] ? doc.seqnum : kCBForestNoSequence;
}


- (BOOL) rawSet: (fdb_doc*)doc error: (NSError**)outError {
    return [self inTransaction: ^BOOL{
        return CHECK_ONQUEUE(fdb_set(self.handle, doc), outError);
    }];
}


- (BOOL) getValue: (NSData**)value meta: (NSData**)meta forKey: (NSData*)key
            error: (NSError**)outError
{
    __block fdb_doc doc = {
        .key = (void*)key.bytes,
        .keylen = key.length,
    };
    __block fdb_status status;
    dispatch_sync(_queue, ^{
        if (value) {
            status = fdb_get(self.handle, &doc);
            *value = SliceToData(doc.body, doc.bodylen);
        } else {
            uint64_t offset;
            status = fdb_get_metaonly(self.handle, &doc, &offset);
        }
    });
    if (status != FDB_RESULT_KEY_NOT_FOUND && !Check(status, outError))
        return NO;
    if (meta)
        *meta = SliceToData(doc.meta, doc.metalen);
    else
        free(doc.meta);
    return YES;
}


// Used only by CBForestDocument
- (fdb_status) rawGetMeta: (fdb_doc*)doc offset: (uint64_t*)outOffset {
    return ONQUEUE(fdb_get_metaonly(self.handle, doc, outOffset));
}

- (fdb_status) rawGetBody: (fdb_doc*)doc byOffset: (uint64_t)offset {
    return ONQUEUE(fdb_get_byoffset(self.handle, doc, offset));
}


- (BOOL) hasValueForKey: (NSData*)key {
    __block fdb_doc doc = {
        .key = (void*)key.bytes,
        .keylen = key.length,
    };
    __block uint64_t offset;
    BOOL result = CHECK_ONQUEUE(fdb_get_metaonly(self.handle, &doc, &offset), NULL);
    free(doc.meta);
    return result;
}


- (BOOL) deleteSequence: (CBForestSequence)sequence error: (NSError**)outError {
    return [self inTransaction: ^BOOL{
        __block fdb_doc doc = {.seqnum = sequence};
        __block fdb_status status;
        dispatch_sync(_queue, ^{
            uint64_t bodyOffset;
            status = fdb_get_metaonly_byseq(self.handle, &doc, &bodyOffset);
            if (status == FDB_RESULT_SUCCESS) {
                doc.body = doc.meta = NULL;
                doc.bodylen = doc.metalen = 0;
                status = fdb_set(self.handle, &doc);
            }
        });
        return status == FDB_RESULT_KEY_NOT_FOUND || Check(status, outError);
    }];
}


// Keys can be NSData (by ID) or NSNumber (by sequence)
- (BOOL) _enumerateValuesFromKey: (id)startKey
                           toKey: (id)endKey
                         options: (const CBForestEnumerationOptions*)options
                           error: (NSError**)outError
                       withBlock: (CBForest_Iterator)block
{
    if (options && options->descending)
        return [self _reverseEnumerateValuesFromKey: startKey
                                              toKey: endKey
                                            options: options
                                              error: outError
                                          withBlock: block];

    fdb_iterator_opt_t fdbOptions = FDB_ITR_METAONLY;
    if (!(options && options->includeDeleted))
        fdbOptions |= FDB_ITR_NO_DELETES;

    __block fdb_iterator *iterator;
    if ([startKey isKindOfClass: [NSNumber class]]) {
        // By sequence:
        CBForestSequence startSequence = [startKey unsignedLongLongValue];
        CBForestSequence endSequence = [endKey unsignedLongLongValue];
        if (options && !options->inclusiveEnd && endSequence > 0)
            endSequence--;
        if (startSequence > self.info.lastSequence || endSequence < startSequence)
            return YES; // no-op
        if (!CHECK_ONQUEUE(fdb_iterator_sequence_init(self.handle, &iterator,
                                                startSequence, endSequence, fdbOptions),
                     outError))
            return NO;
    } else {
        // By ID:
        const void* endKeyBytes = [endKey bytes];
        size_t endKeyLength = [endKey length];
        if (options && !options->inclusiveEnd && endKeyBytes)
            block = ^BOOL(fdb_doc *doc, uint64_t bodyOffset) {
                if (doc->keylen == endKeyLength && memcmp(doc->key, endKeyBytes, endKeyLength)==0)
                    return false;   // stop _before_ the endKey
                return block(doc, bodyOffset);
            };
        if (!CHECK_ONQUEUE(fdb_iterator_init(self.handle, &iterator,
                                       [startKey bytes], [startKey length],
                                       endKeyBytes, endKeyLength, fdbOptions),
                     outError))
            return NO;
    }

    __block unsigned skip  = options ? options->skip  : 0;
    __block unsigned limit = options ? options->limit : 0;
                          for (;;) {
                            __block fdb_doc *docinfo;
        __block uint64_t bodyOffset;
        fdb_status status = ONQUEUE(fdb_iterator_next_offset(iterator, &docinfo, &bodyOffset));
        if (status != FDB_RESULT_SUCCESS || docinfo == NULL)
            break; // FDB returns FDB_RESULT_FAIL at end of iteration

        if (skip > 0) {
            skip--;
            continue;
        }

        if (!block(docinfo, bodyOffset))
            break;
        // We assume the block either freed docinfo or will free it later.

        if (limit > 0 && --limit == 0)
            break;
    }
    dispatch_sync(_queue, ^{
        fdb_iterator_close(iterator);
    });
    return YES;
}


- (BOOL) _reverseEnumerateValuesFromKey: (id)startKey
                                  toKey: (id)endKey
                                options: (const CBForestEnumerationOptions*)options
                                  error: (NSError**)outError
                              withBlock: (CBForest_Iterator)block
{
    @autoreleasepool {
        // Handle descending mode by buffering up all the docs:
        CBForestEnumerationOptions fwdOptions = *options;
        fwdOptions.descending = NO;
        NSMutableArray* docs = [[NSMutableArray alloc] init];
        NSMutableArray* offsets = [[NSMutableArray alloc] init];
        BOOL ok = [self _enumerateValuesFromKey: endKey toKey: startKey options: &fwdOptions
                                          error: outError
                                      withBlock: ^BOOL(fdb_doc *doc, uint64_t bodyOffset)
        {
            [docs addObject: [NSValue valueWithPointer: doc]];
            [offsets addObject: @(bodyOffset)];
            return true;
        }];
        if (!ok)
            return NO;

        for (NSInteger i = docs.count-1; i >= 0; i--) {
            fdb_doc *doc = [docs[i] pointerValue];
            uint64_t bodyOffset = [offsets[i] unsignedLongLongValue];
            if (!block(doc, bodyOffset))
                break;
        }
        return YES;
    }
}


- (BOOL) enumerateValuesFromKey: (NSData*)startKey
                          toKey: (NSData*)endKey
                        options: (const CBForestEnumerationOptions*)options
                          error: (NSError**)outError
                      withBlock: (CBForestValueIterator)block
{
    return [self _enumerateValuesFromKey: startKey toKey: endKey options: options error: outError
                               withBlock: ^BOOL(fdb_doc *doc, uint64_t bodyOffset)
    {
        @autoreleasepool {
            NSData* key = SliceToData(doc->key, doc->keylen);
            NSData* value = SliceToData(doc->body, doc->bodylen);
            NSData* meta = SliceToData(doc->meta, doc->metalen);
            fdb_doc_free(doc);
            BOOL stop = NO;
            block(key, value, meta, &stop);
            return !stop;
        }
    }];
}


#pragma mark - DOCUMENTS:


- (CBForestDocument*) makeDocumentWithID: (NSString*)docID {
    return [[_documentClass alloc] initWithDB: self docID: docID];
}


- (CBForestDocument*) documentWithID: (NSString*)docID
                             options: (CBForestContentOptions)options
                               error: (NSError**)outError
{
    CBForestDocument* doc = [[_documentClass alloc] initWithDB: self docID: docID];
    if (![doc reload: options error: outError])
        return nil;
    if (!(options & kCBForestDBCreateDoc) && !doc.exists) {
        Check(FDB_RESULT_KEY_NOT_FOUND, outError);
        return nil;
    }
    return doc;
}


- (CBForestDocument*) documentWithSequence: (CBForestSequence)sequence
                                   options: (CBForestContentOptions)options
                                     error: (NSError**)outError
{
    __block fdb_doc doc = {
        .seqnum = sequence
    };
    __block uint64_t bodyOffset = 0;
    if (!CHECK_ONQUEUE(fdb_get_metaonly_byseq(self.handle, &doc, &bodyOffset), outError))
        return nil;
    return [[_documentClass alloc] initWithDB: self
                                         info: &doc
                                       offset: bodyOffset
                                      options: options
                                        error: outError];
}


- (BOOL) deleteDocument: (CBForestDocument*)doc error: (NSError**)outError {
    return [self setValue: nil meta: nil
                   forKey: [doc.docID dataUsingEncoding: NSUTF8StringEncoding]
                    error: outError] != kCBForestNoSequence;
}


#pragma mark - ITERATION:


- (BOOL) _enumerateDocsFromKey: (id)startKey
                         toKey: (id)endKey
                       options: (const CBForestEnumerationOptions*)options
                         error: (NSError**)outError
                     withBlock: (CBForestDocIterator)block
{
    CBForestContentOptions contentOptions = (options ? options->contentOptions : 0);
    __block BOOL docOK = YES;
    BOOL ok = [self _enumerateValuesFromKey: startKey
                                      toKey: endKey
                                    options: options
                                      error: outError
                                  withBlock: ^BOOL(fdb_doc *docinfo, uint64_t bodyOffset)
    {
        if (![_documentClass docInfo: docinfo matchesOptions: options]) {
            fdb_doc_free(docinfo);
            return true;
        }
        @autoreleasepool {
            CBForestDocument* doc = [[_documentClass alloc] initWithDB: self
                                                                  info: docinfo
                                                                offset: bodyOffset
                                                               options: contentOptions
                                                                 error: outError];
            if (!doc) {
                docOK = NO;
                return NO;
            }
            BOOL stop = NO;
            block(doc, &stop);
            return !stop;
        }
    }];
    return ok && docOK;
}


- (BOOL) enumerateDocsFromID: (NSString*)startID
                        toID: (NSString*)endID
                     options: (const CBForestEnumerationOptions*)options
                       error: (NSError**)outError
                   withBlock: (CBForestDocIterator)block
{
    return [self _enumerateDocsFromKey: [startID dataUsingEncoding: NSUTF8StringEncoding]
                                 toKey: [endID dataUsingEncoding: NSUTF8StringEncoding]
                               options: options error: outError withBlock: block];
}


- (BOOL) enumerateDocsFromSequence: (CBForestSequence)startSequence
                        toSequence: (CBForestSequence)endSequence
                           options: (const CBForestEnumerationOptions*)options
                             error: (NSError**)outError
                         withBlock: (CBForestDocIterator)block
{
    return [self _enumerateDocsFromKey: @(startSequence) toKey: @(endSequence)
                               options: options error: outError withBlock: block];
}


- (NSString*) dump {
    NSMutableString* dump = [NSMutableString stringWithCapacity: 1000];
    [self enumerateDocsFromID: nil toID: nil options: 0 error: NULL
                    withBlock: ^(CBForestDocument *doc, BOOL *stop)
    {
        [dump appendFormat: @"\t\"%@\": %lu meta, %llu body\n",
                             doc.docID, (unsigned long)doc.metadata.length, doc.bodyLength];
    }];
    return dump;
}


@end
