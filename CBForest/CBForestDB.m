//
//  CBForest.m
//  CBForest
//
//  Created by Jens Alfke on 9/4/12.
//  Copyright (c) 2012 Couchbase, Inc. All rights reserved.
//

#import "CBForestDB.h"
#import "CBForestPrivate.h"
#import "CBEnumerator.h"
#import "slice.h"
#import <libkern/OSAtomic.h>


//#define ASYNC_COMMIT
#define LOGGING

#define kEnumeratorBufferSize 16

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
    CBForestSequence _transactionStartSequence;
    NSError* _transactionError;
    dispatch_queue_t _queue;
    CBForestToken* _writeLock;
}

@synthesize documentClass=_documentClass;


#ifdef LOGGING
static void forestdbLog(int err_code, const char *err_msg, void *ctx_data) {
    [(__bridge CBForestDB*)ctx_data logError: err_code message: err_msg];
}

- (void) logError: (int)status message: (const char*)message {
    NSLog(@"ForestDB error %d: %s", status, message);
}
#endif


- (id) initWithFile: (NSString*)filePath
            options: (CBForestFileOptions)options
             config: (const CBForestDBConfig*)config
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
    }
    return self;
}

- (id) initWithFile: (NSString*)filePath
            options: (CBForestFileOptions)options
             config: (const CBForestDBConfig*)config
              error: (NSError**)outError
{
    self = [self initWithFile: filePath options: options config: config];
    if (self) {
        if (![self open: filePath options: options error: outError])
            return nil;
#ifdef LOGGING
        fdb_set_log_callback(_db, &forestdbLog, (__bridge void*)(self));
#endif
    }
    return self;
}

- (id) initWithFile: (NSString*)filePath
            options: (CBForestFileOptions)options
             config: (const CBForestDBConfig*)config
           snapshot: (fdb_handle*)snapshot
{
    self = [self initWithFile: filePath options: options config: config];
    if (self) {
        _db = snapshot;
#ifdef LOGGING
        fdb_set_log_callback(_db, &forestdbLog, (__bridge void*)(self));
#endif
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

//FIX: This was left out of forestdb.h (MB-11078)
extern void set_default_fdb_config(fdb_config *fconfig);

- (BOOL) open: (NSString*)filePath
      options: (CBForestFileOptions)options
        error: (NSError**)outError
{
    NSAssert(!_db, @"Already open");

    __block fdb_status status;
    dispatch_sync(_queue, ^{
        fdb_config config;
        set_default_fdb_config(&config);
        if (_customConfig) {
            config.flags                    = (fdb_open_flags)options,
            config.buffercache_size         = _config.bufferCacheSize;
            config.wal_threshold            = _config.walThreshold;
            config.seqtree_opt              = _config.enableSequenceTree;
            config.compress_document_body   = _config.compressDocBodies;
        }
        // ForestDB doesn't yet pay any attention to the FDB_OPEN_FLAG_CREATE flag (MB-11079)
        if ((options & FDB_OPEN_FLAG_CREATE)
                || [[NSFileManager defaultManager] fileExistsAtPath: filePath])
            status = fdb_open(&_db, filePath.fileSystemRepresentation, &config);
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

- (BOOL) isReadOnly {
    return (_openFlags & kCBForestDBReadOnly) != 0;
}

- (BOOL) commit: (NSError**)outError {
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


- (BOOL) rollbackToSequence: (CBForestSequence)oldSequence error: (NSError**)outError {
    return CHECK_ONQUEUE(fdb_rollback(&_db, oldSequence), outError);
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


- (CBForestDB*) openSnapshotAtSequence: (CBForestSequence)sequence
                                 error: (NSError**)outError
{
    __block fdb_handle *snapshotHandle;
    if (!CHECK_ONQUEUE(fdb_snapshot_open(self.handle, &snapshotHandle, sequence), outError))
        return nil;
    return [[[self class] alloc] initWithFile: self.filename
                                      options: _openFlags | kCBForestDBReadOnly
                                       config: &_config
                                     snapshot: snapshotHandle];
}


#pragma mark - TRANSACTIONS:


- (BOOL) isInTransaction {
    @synchronized(self) {
        return _transactionLevel > 0;
    }
}


- (void) beginTransaction {
    @synchronized(self) {
        if (++_transactionLevel == 1) {
            [_writeLock lockWithOwner: self];
            _transactionStartSequence = self.info.lastSequence;
            _transactionError = nil;
        }
    }
}


- (void) noteTransactionError: (NSError*)error {
    if (error) {
        @synchronized(self) {
            if (_transactionLevel > 0 && !_transactionError)
                _transactionError = error;
        }
    }
}


- (void) failTransaction {
    [self noteTransactionError: [NSError errorWithDomain: CBForestErrorDomain
                                                    code: kCBForestErrorTransactionAborted
                                                userInfo: nil]];
}


- (BOOL) endTransaction: (NSError**)outError {
    @synchronized(self) {
        NSError* error;
        if (_transactionLevel == 1) {
            // Ending the outermost transaction:
            // Use an empty dispatch_sync to ensure any async operations complete:
            dispatch_sync(_queue, ^{ });
            dispatch_sync(_queue, ^{ });
            // Get the transaction error status:
            error = _transactionError;
            _transactionError = nil;
            if (_db) {
                // Commit or roll back:
                if (error || ![self commit: &error])
                    [self rollbackToSequence: _transactionStartSequence error: NULL];
            }
            _transactionStartSequence = 0;
        } else {
            error = _transactionError;
        }
        if (outError)
            *outError = error;
        --_transactionLevel;
        return (error == nil);
    }
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
        if (!ok)
            [self failTransaction];
        ok = [self endTransaction: NULL];
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
        .metalen = meta.length,
        .deleted = (value == nil)
    };
    return [self rawSet: &doc error: outError] ? doc.seqnum : kCBForestNoSequence;
}


- (BOOL) rawSet: (fdb_doc*)doc error: (NSError**)outError {
    return [self inTransaction: ^BOOL{
        return CHECK_ONQUEUE(fdb_set(self.handle, doc), outError);
    }];
}


- (void) asyncSetValue: (NSData*)value meta: (NSData*)meta forKey: (NSData*)key
            onComplete: (void(^)(CBForestSequence,NSError*))onComplete
{
    NSAssert(_transactionLevel > 0, @"Must be called in transaction"); //FIX: thread-safety
    dispatch_async(_queue, ^{
        fdb_doc doc = {
            .key = (void*)key.bytes,
            .keylen = key.length,
            .body = (void*)value.bytes,
            .bodylen = value.length,
            .meta = (void*)meta.bytes,
            .metalen = meta.length,
            .deleted = (value == nil)
        };
        NSError* error;
        if (CheckWithKey(fdb_set(_db, &doc), key, &error)) {
            if (onComplete)
                onComplete(doc.seqnum, nil);
        } else {
            if (!_transactionError)
                _transactionError = error;
            if (onComplete)
                onComplete(0, error);
        }
    });
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
            status = fdb_get_metaonly(self.handle, &doc);
        }
    });
    if (status != FDB_RESULT_KEY_NOT_FOUND && !CheckWithKey(status, key, outError))
        return NO;
    if (meta)
        *meta = SliceToData(doc.meta, doc.metalen);
    else
        free(doc.meta);
    return YES;
}


// Used only by CBForestDocument
- (fdb_status) rawGet: (fdb_doc*)doc options: (CBForestContentOptions)options {
    __block fdb_status status;
    dispatch_sync(_queue, ^{
        if (options & kCBForestDBMetaOnly)
            status = fdb_get_metaonly(self.handle, doc);
        else if (doc->offset)
            status = fdb_get_byoffset(self.handle, doc);
        else
            status = fdb_get(self.handle, doc);
    });
    return status;
}


- (BOOL) hasValueForKey: (NSData*)key {
    __block fdb_doc doc = {
        .key = (void*)key.bytes,
        .keylen = key.length,
    };
    BOOL result = CHECK_ONQUEUE(fdb_get_metaonly(self.handle, &doc), NULL);
    free(doc.meta);
    return result;
}


- (BOOL) deleteSequence: (CBForestSequence)sequence error: (NSError**)outError {
    return [self inTransaction: ^BOOL{
        __block fdb_doc doc = {.seqnum = sequence};
        __block fdb_status status;
        dispatch_sync(_queue, ^{
            status = fdb_get_metaonly_byseq(self.handle, &doc);
            if (status == FDB_RESULT_SUCCESS) {
                doc.body = doc.meta = NULL;
                doc.bodylen = doc.metalen = 0;
                doc.deleted = true;
                status = fdb_set(self.handle, &doc);
            }
        });
        return status == FDB_RESULT_KEY_NOT_FOUND || Check(status, outError);
    }];
}


- (void) asyncDeleteSequence: (CBForestSequence)sequence {
    NSAssert(self.isInTransaction, @"Must be called in transaction");
    dispatch_async(_queue, ^{
        fdb_doc doc = {.seqnum = sequence};
        fdb_status status;
        status = fdb_get_metaonly_byseq(self.handle, &doc);
        if (status == FDB_RESULT_SUCCESS) {
            doc.body = doc.meta = NULL;
            doc.bodylen = doc.metalen = 0;
            doc.deleted = true;
            status = fdb_set(self.handle, &doc);
        }
        NSError* error;
        if (!Check(status, &error)) {
            if (!_transactionError)
                _transactionError = error;
        }
    });
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
        CheckWithKey(FDB_RESULT_KEY_NOT_FOUND, docID, outError);
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
    BOOL metaOnly = (options & kCBForestDBMetaOnly) != 0;
    if (!CHECK_ONQUEUE(metaOnly ? fdb_get_metaonly_byseq(self.handle, &doc)
                                : fdb_get_byseq(self.handle, &doc),
                       outError))
        return nil;
    return [[_documentClass alloc] initWithDB: self
                                         info: &doc
                                      options: options
                                        error: outError];
}


- (BOOL) deleteDocument: (CBForestDocument*)doc error: (NSError**)outError {
    return [self setValue: nil meta: nil
                   forKey: [doc.docID dataUsingEncoding: NSUTF8StringEncoding]
                    error: outError] != kCBForestNoSequence;
}


#pragma mark - ITERATION:


- (NSEnumerator*) enumerateDocsFromID: (NSString*)startID
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


- (NSEnumerator*) enumerateDocsFromKey: (NSData*)startKey
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


- (NSEnumerator*) enumerateDocsFromSequence: (CBForestSequence)startSequence
                                 toSequence: (CBForestSequence)endSequence
                                    options: (const CBForestEnumerationOptions*)options
                                      error: (NSError**)outError
{
    if (options && !options->inclusiveEnd && endSequence > 0)
        endSequence--;
    if (startSequence > self.info.lastSequence || endSequence < startSequence)
        return @[].objectEnumerator; // no-op; return empty enumerator
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


- (NSEnumerator*) enumerateDocsWithKeys: (NSArray*)keys
                                options: (const CBForestEnumerationOptions*)optionsP
                                  error: (NSError**)outError
{
    __block CBForestEnumerationOptions options = optionsP ? *optionsP
                                                          : kCBForestEnumerationOptionsDefault;
    NSEnumerator* keyEnum;
    if (options.descending)
        keyEnum = keys.reverseObjectEnumerator;
    else
        keyEnum = keys.objectEnumerator;

    CBEnumeratorBlock enumerator = ^id {
        // Get a fdb_doc from ForestDB:
        NSString* docID = keyEnum.nextObject;
        if (!docID)
            return nil;
        if ([docID isKindOfClass: [NSData class]])
            docID = [[NSString alloc] initWithData: (NSData*)docID encoding: NSUTF8StringEncoding];
        return [self documentWithID: docID
                            options: options.contentOptions | kCBForestDBCreateDoc
                              error: NULL];
    };
    return CBEnumeratorBlockToObject(CBBufferedEnumerator(kEnumeratorBufferSize, enumerator));
}


- (NSEnumerator*) enumeratorForIterator: (fdb_iterator*)iterator
                                options: (const CBForestEnumerationOptions*)optionsP
                                 endKey: (NSData*)endKey
{
    __block CBForestEnumerationOptions options = optionsP ? *optionsP
                                                          : kCBForestEnumerationOptionsDefault;
    BOOL metaOnly = (options.contentOptions & kCBForestDBMetaOnly) != 0;
    __block BOOL done = NO;
    CBEnumeratorBlock enumerator = ^id {
        if (done)
            return nil;
        CBForestDocument* result = nil;
        do {
            // Get a fdb_doc from ForestDB:
            __block fdb_doc *docinfo;
            fdb_status status = metaOnly ? fdb_iterator_next_metaonly(iterator, &docinfo)
                                         : fdb_iterator_next(iterator, &docinfo);
            if (status != FDB_RESULT_SUCCESS)
                break;

            if (options.skip > 0) {
                // Skip this one
                --options.skip;
                fdb_doc_free(docinfo);
            } else if (!options.inclusiveEnd && endKey
                            && 0 == slicecmp(DataToSlice(endKey),
                                             (slice){docinfo->key, docinfo->keylen})) {
                // Stop before this key, i.e. this is the endKey and inclusiveEnd is false
                fdb_doc_free(docinfo);
                break;
            } else if (![_documentClass docInfo: docinfo matchesOptions: &options]) {
                // Nope, this doesn't match the options (i.e. it's deleted or something)
                fdb_doc_free(docinfo);
            } else {
                // Whew! Finally found a doc to return...
                result = [[_documentClass alloc] initWithDB: self
                                                       info: docinfo
                                                    options: options.contentOptions
                                                      error: NULL];
                free(docinfo);
            }
        } while (!result);

        if (!result || (options.limit > 0 && --options.limit == 0)) {
            fdb_iterator_close(iterator);
            done = YES;
        }
        return result;
    };

    if (options.descending)
        return CBEnumeratorBlockReversedToObject(enumerator);
    else
        return CBEnumeratorBlockToObject(CBBufferedEnumerator(kEnumeratorBufferSize, enumerator));
}


- (NSString*) dump {
    NSMutableString* dump = [NSMutableString stringWithCapacity: 1000];
    NSEnumerator* e = [self enumerateDocsFromID: nil toID: nil options: 0 error: NULL];
    for (CBForestDocument* doc in e) {
        [dump appendFormat: @"\t\"%@\": %lu meta, %llu body\n",
                             doc.docID, (unsigned long)doc.metadata.length, doc.bodyLength];
    }
    return dump;
}


@end
