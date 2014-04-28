//
//  CBForest.m
//  CBForest
//
//  Created by Jens Alfke on 9/4/12.
//  Copyright (c) 2012 Couchbase, Inc. All rights reserved.
//

#import "CBForestDB.h"
#import "CBForestPrivate.h"
#import "CBForestDocEnumerator.h"
#import "slice.h"
#import <libkern/OSAtomic.h>


//#define ASYNC_COMMIT

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


const CBForestEnumerationOptions kCBForestEnumerationOptionsDefault = {
    .inclusiveEnd = YES
};


@implementation CBForestDB
{
    fdb_handle *_db;
    fdb_open_flags _openFlags;
    CBForestDBConfig _config;
    BOOL _customConfig;
    int32_t _transactionLevel;
    dispatch_queue_t _queue;
    CBForestToken* _writeLock;
}

@synthesize documentClass=_documentClass;


- (id) initWithFile: (NSString*)filePath
            options: (CBForestFileOptions)options
             config: (const CBForestDBConfig*)config
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
        if (config) {
            _customConfig = YES;
            _config = *config;
        }
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

static NSDictionary* mkConfig(id value) {
    return @{@"default": value,
             @"validator": @{@"range": @{@"min": @0, @"max": @(1.0e30)}}};
}

- (BOOL) open: (NSString*)filePath
      options: (CBForestFileOptions)options
        error: (NSError**)outError
{
    NSAssert(!_db, @"Already open");

    NSString* configPath = nil;
    if (_customConfig) {
        NSDictionary* config = @{@"buffer_cache_size": mkConfig(@(_config.bufferCacheSize)),
                                 @"wal_threshold":     mkConfig(@(_config.walThreshold)),
                                 @"enable_sequence_tree": mkConfig(_config.enableSequenceTree? @YES : @NO),
                                 @"compress_document_body": mkConfig(_config.compressDocBodies ? @YES : @NO)};
        configPath = [filePath stringByAppendingString: @".config"];
        NSDictionary* rootConfig = @{@"configs": config};
        NSData* json = [NSJSONSerialization dataWithJSONObject: rootConfig options: 0 error: NULL];
        [json writeToFile: configPath atomically: YES];
    }

    __block fdb_status status;
    dispatch_sync(_queue, ^{
        // ForestDB doesn't yet pay any attention to the FDB_OPEN_FLAG_CREATE flag. --4/2014
        if ((options & FDB_OPEN_FLAG_CREATE)
                || [[NSFileManager defaultManager] fileExistsAtPath: filePath])
            status = fdb_open(&_db, filePath.fileSystemRepresentation, options,
                              configPath.fileSystemRepresentation);
        else
            status = FDB_RESULT_NO_SUCH_FILE;
    });

    if (configPath)
        [[NSFileManager defaultManager] removeItemAtPath: configPath error: NULL];

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
//  NSLog(@"~~~~~ COMMIT ~~~~~");
    if (_openFlags & FDB_OPEN_FLAG_RDONLY)
        return YES; // no-op if read-only
#ifdef ASYNC_COMMIT
    dispatch_async(_queue, ^{
        fdb_status status = fdb_commit(_db, FDB_COMMIT_NORMAL);
        if (status != FDB_RESULT_SUCCESS)
            NSLog(@"WARNING: fdb_commit failed, status=%d", status);
    });
    return YES;
#else
    return CHECK_ONQUEUE(fdb_commit(_db, FDB_COMMIT_NORMAL), outError);
#endif
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
            && [self open: path
                  options: (_openFlags | kCBForestDBCreate)
                    error: outError];
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
    dispatch_sync(_queue, ^{
        if (OSAtomicIncrement32(&_transactionLevel) == 1)
            [_writeLock lockWithOwner: self];
    });
}


- (BOOL) endTransaction: (NSError**)outError {
    BOOL result = YES;
    if (OSAtomicDecrement32(&_transactionLevel) == 0) {
        // Ending the outermost transaction:
        if (_db)
            result = [self commit: outError];
    }
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
            *value = SliceToAdoptingData(doc.body, doc.bodylen);
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
    return ONQUEUE(offset ? fdb_get_byoffset(self.handle, doc, offset) : fdb_get(self.handle, doc));
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


- (CBForestEnumerator*) enumerateDocsFromID: (NSString*)startID
                                       toID: (NSString*)endID
                                    options: (const CBForestEnumerationOptions*)options
                                      error: (NSError**)outError
{
    return [self enumerateDocsFromKey: [startID dataUsingEncoding: NSUTF8StringEncoding]
                                toKey: [endID dataUsingEncoding: NSUTF8StringEncoding]
                              options: options error: outError];
}


static fdb_iterator_opt_t iteratorOptions(const CBForestEnumerationOptions* options) {
    fdb_iterator_opt_t fdbOptions = 0;
    if (options && (options->contentOptions & kCBForestDBMetaOnly))
        fdbOptions |= FDB_ITR_METAONLY;
    if (!(options && options->includeDeleted))
        fdbOptions |= FDB_ITR_NO_DELETES;
    return fdbOptions;
}


- (CBForestEnumerator*) enumerateDocsFromKey: (NSData*)startKey
                                       toKey: (NSData*)endKey
                                     options: (const CBForestEnumerationOptions*)options
                                       error: (NSError**)outError
{
    if (options && options->descending) {
        NSData* temp = startKey;
        startKey = endKey;
        endKey = temp;
    }
    __block fdb_iterator *iterator;
    if (!CHECK_ONQUEUE(fdb_iterator_init(self.handle, &iterator,
                                         startKey.bytes, startKey.length,
                                         endKey.bytes, endKey.length, iteratorOptions(options)),
                       outError))
        return nil;
    return [self enumeratorForIterator: iterator options: options endKey: endKey];
}


- (CBForestEnumerator*) enumerateDocsFromSequence: (CBForestSequence)startSequence
                                       toSequence: (CBForestSequence)endSequence
                                          options: (const CBForestEnumerationOptions*)options
                                            error: (NSError**)outError
{
    if (options && !options->inclusiveEnd && endSequence > 0)
        endSequence--;
    if (startSequence > self.info.lastSequence || endSequence < startSequence)
        return [[CBForestDocEnumerator alloc] init]; // no-op; return empty enumerator
    if (options && options->descending) {
        CBForestSequence temp = startSequence;
        startSequence = endSequence;
        endSequence = temp;
    }
    __block fdb_iterator *iterator;
    if (!CHECK_ONQUEUE(fdb_iterator_sequence_init(self.handle, &iterator,
                                                  startSequence, endSequence,
                                                  iteratorOptions(options)),
                       outError))
        return nil;
    return [self enumeratorForIterator: iterator options: options endKey: nil];
}


static fdb_status keysNextMultiple(fdb_handle *db, NSEnumerator* keyEnum, BOOL metaOnly,
                                   unsigned *n, fdb_doc** docinfos, uint64_t *bodyOffsets)
{
    fdb_status status = FDB_RESULT_SUCCESS;
    unsigned i;
    for (i=0; i<*n; i++) {
        NSData* key = keyEnum.nextObject;
        if (!key) {
            status = FDB_RESULT_KEY_NOT_FOUND;
            break;
        } else if ([key isKindOfClass: [NSString class]]) {
            key = [(NSString*)key dataUsingEncoding: NSUTF8StringEncoding];
        }
        fdb_doc* doc;
        fdb_doc_create(&doc, key.bytes, key.length, NULL, 0, NULL, 0);
        status = fdb_get_metaonly(db, doc, &bodyOffsets[i]);
        if (status == FDB_RESULT_KEY_NOT_FOUND) {
            bodyOffsets[i] = 0;
        } else {
            if (status == FDB_RESULT_SUCCESS && !metaOnly)
                status = fdb_get_byoffset(db, doc, bodyOffsets[i]);
            if (status != FDB_RESULT_SUCCESS) {
                fdb_doc_free(doc);
                break;
            }
        }
        docinfos[i] = doc;
    }
    *n = i;
    return status;
}


- (CBForestEnumerator*) enumerateDocsWithKeys: (NSArray*)keys
                                      options: (const CBForestEnumerationOptions*)options
                                        error: (NSError**)outError
{
    BOOL metaOnly = options && (options->contentOptions & kCBForestDBMetaOnly);
    NSEnumerator* keyEnum;
    if (options && options->descending)
        keyEnum = keys.reverseObjectEnumerator;
    else
        keyEnum = keys.objectEnumerator;

    return [[CBForestDocEnumerator alloc] initWithDatabase: self
                                                   options: options
                                                    endKey: nil
                                                 nextBlock:
            ^fdb_status(unsigned *n, fdb_doc** docinfos, uint64_t *bodyOffsets) {
                return ONQUEUE(keysNextMultiple(_db, keyEnum, metaOnly, n, docinfos, bodyOffsets));
            }
                                               finishBlock: nil];
}


static fdb_status iteratorNextMultiple(fdb_handle *db, fdb_iterator *iterator, BOOL metaOnly,
                                       unsigned *n, fdb_doc** docinfos, uint64_t *bodyOffsets)
{
    fdb_status status = FDB_RESULT_SUCCESS;
    unsigned i;
    for (i=0; i<*n; i++) {
        status = fdb_iterator_next_offset(iterator, &docinfos[i], &bodyOffsets[i]);
        if (status == FDB_RESULT_SUCCESS && !metaOnly)
            status = fdb_get_byoffset(db, docinfos[i], bodyOffsets[i]);
        if (status != FDB_RESULT_SUCCESS)
            break;
    }
    *n = i;
    return status;
}


- (CBForestEnumerator*) enumeratorForIterator: (fdb_iterator*)iterator
                                      options: (const CBForestEnumerationOptions*)options
                                       endKey: (NSData*)endKey
{
    BOOL metaOnly = options && (options->contentOptions & kCBForestDBMetaOnly);
    CBForestEnumerator* e;
    e = [[CBForestDocEnumerator alloc] initWithDatabase: self
                                                options: options
                                                 endKey: endKey
                                              nextBlock:
            ^fdb_status(unsigned *n, fdb_doc** docinfos, uint64_t *bodyOffsets) {
                return ONQUEUE(iteratorNextMultiple(_db, iterator, metaOnly, n, docinfos, bodyOffsets));
            }
                                            finishBlock:
            ^{
                dispatch_async(_queue, ^{
                    fdb_iterator_close(iterator);
                });
            }];
    if (options && options->descending)
        e = [[CBForestReverseEnumerator alloc] initWithEnumerator: e];
    return e;
}


- (NSString*) dump {
    NSMutableString* dump = [NSMutableString stringWithCapacity: 1000];
    CBForestEnumerator* e = [self enumerateDocsFromID: nil toID: nil options: 0 error: NULL];
    for (CBForestDocument* doc in e) {
        [dump appendFormat: @"\t\"%@\": %lu meta, %llu body\n",
                             doc.docID, (unsigned long)doc.metadata.length, doc.bodyLength];
    }
    return dump;
}


@end
