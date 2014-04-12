//
//  CBForest.m
//  CBForest
//
//  Created by Jens Alfke on 9/4/12.
//  Copyright (c) 2012 Couchbase, Inc. All rights reserved.
//

#import "CBForestDB.h"
#import "CBForestPrivate.h"
#import "rev_tree.h"


NSString* const CBForestErrorDomain = @"CBForest";


@implementation CBForestDB
{
    fdb_handle *_db;
    fdb_open_flags _openFlags;
}

@synthesize documentClass=_documentClass;


- (id) initWithFile: (NSString*)filePath
            options: (CBForestFileOptions)options
              error: (NSError**)outError
{
    self = [super init];
    if (self) {
        _documentClass = [CBForestDocument class];
        assert(kCBForestDBCreate == FDB_OPEN_FLAG_CREATE);
        assert(kCBForestDBReadOnly == FDB_OPEN_FLAG_RDONLY);
        _openFlags = (fdb_open_flags)options;
        // Note: ForestDB doesn't yet pay any attention to the FDB_OPEN_FLAG_CREATE flag. --4/2014
        if (![self open: filePath error: outError])
            return nil;
    }
    return self;
}

- (void) dealloc {
    fdb_close(_db);
}

- (BOOL) open: (NSString*)filePath error: (NSError**)outError {
    NSAssert(!_db, @"Already open");
    return Check(fdb_open(&_db, filePath.fileSystemRepresentation, _openFlags, NULL), outError);
}

- (fdb_handle*) handle {
    NSAssert(_db, @"Already closed!");
    return _db;
}

- (void) close {
    fdb_close(_db);
    _db = nil;
}

- (NSString*) description {
    return [NSString stringWithFormat: @"%@[%@]", [self class], self.filename];
}

- (NSString*) filename {
    fdb_info info;
    fdb_get_dbinfo(self.handle, &info);
    return [[NSFileManager defaultManager] stringWithFileSystemRepresentation: info.filename
                                                                    length: strlen(info.filename)];
}

- (CBForestDBInfo) info {
    fdb_info info;
    fdb_get_dbinfo(self.handle, &info);
    return (CBForestDBInfo) {
        //.headerRevNum = _db.cur_header_revnum,
        .dataSize       = info.space_used,
        .fileSize       = info.file_size,
        .documentCount  = info.doc_count,
        .lastSequence   = info.last_seqnum,
    };
}

- (BOOL) commit: (NSError**)outError {
    return Check(fdb_commit(self.handle), outError)
        && Check(fdb_flush_wal(_db), outError);
}

- (BOOL) compact: (NSError**)outError
{
    NSString* filename = self.filename;
    NSString* tempFile = [filename stringByAppendingPathExtension: @"cpt"];
    [[NSFileManager defaultManager] removeItemAtPath: tempFile error: NULL];
    if (!Check(fdb_compact(self.handle, tempFile.fileSystemRepresentation), outError)) {
        [[NSFileManager defaultManager] removeItemAtPath: tempFile error: NULL];
        return NO;
    }
    // Now replace the original file with the compacted one and re-open it:
    [self close];
    if (rename(tempFile.fileSystemRepresentation, filename.fileSystemRepresentation) < 0) {
        if (outError)
            *outError = [NSError errorWithDomain: NSPOSIXErrorDomain code: errno userInfo: nil];
        [[NSFileManager defaultManager] removeItemAtPath: tempFile error: NULL];
        [self open: filename error: NULL];
        return NO;
    }
    return [self open: filename error: outError];
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
    if (!Check(fdb_set(self.handle, &doc), outError))
        return kCBForestNoSequence;
    return doc.seqnum;
}


- (BOOL) getValue: (NSData**)value meta: (NSData**)meta forKey: (NSData*)key error: (NSError**)outError {
    fdb_doc doc = {
        .key = (void*)key.bytes,
        .keylen = key.length,
    };
    fdb_status status;
    if (value) {
        status = fdb_get(self.handle, &doc);
        *value = BufToData(doc.body, doc.bodylen);
    } else {
        status = fdb_get_metaonly(self.handle, &doc, NULL);
    }
    if (status != FDB_RESULT_KEY_NOT_FOUND && !Check(status, outError))
        return NO;
    if (meta)
        *meta = BufToData(doc.meta, doc.metalen);
    return YES;
}


- (BOOL) deleteSequence: (CBForestSequence)sequence error: (NSError**)outError {
    fdb_doc doc = {.seqnum = sequence};
    uint64_t bodyOffset;
    fdb_status status = fdb_get_metaonly_byseq(self.handle, &doc, &bodyOffset);
    if (status == FDB_RESULT_KEY_NOT_FOUND)
        return YES;
    else if (!Check(status, outError))
        return NO;
    doc.body = doc.meta = NULL;
    doc.bodylen = doc.metalen = 0;
    return Check(fdb_set(self.handle, &doc), outError);
}


- (BOOL) _enumerateValuesFromKey: (NSData*)startKey
                           toKey: (NSData*)endKey
                         options: (const CBForestEnumerationOptions*)options
                           error: (NSError**)outError
                       withBlock: (CBForest_Iterator)block
{
    fdb_iterator_opt_t fdbOptions = FDB_ITR_METAONLY;
    if (!(options && options->includeDeleted))
        fdbOptions |= FDB_ITR_NO_DELETES;
    const void* endKeyBytes = endKey.bytes;
    size_t endKeyLength = endKey.length;
    fdb_iterator *iterator;
    fdb_status status = fdb_iterator_init(self.handle, &iterator,
                                          startKey.bytes, startKey.length,
                                          endKeyBytes, endKeyLength, fdbOptions);
    if (!Check(status, outError))
        return NO;

    __block unsigned skip  = options ? options->skip  : 0;
    __block unsigned limit = options ? options->limit : 0;
    for (;;) {
        fdb_doc *docinfo;
        uint64_t bodyOffset;
        status = fdb_iterator_next_offset(iterator, &docinfo, &bodyOffset);
        if (status != FDB_RESULT_SUCCESS || docinfo == NULL)
            break; // FDB returns FDB_RESULT_FAIL at end of iteration

        if (skip > 0) {
            skip--;
            continue;
        }
        if (options && !options->inclusiveEnd && endKeyBytes
                && docinfo->keylen == endKeyLength
                && memcmp(docinfo->key, endKeyBytes, endKeyLength)==0)
            break;

        if (!block(docinfo, bodyOffset))
            break;

        if (limit > 0 && --limit == 0)
            break;
    }
    fdb_iterator_close(iterator);
    return YES;
}


- (BOOL) enumerateValuesFromKey: (NSData*)startKey
                          toKey: (NSData*)endKey
                        options: (const CBForestEnumerationOptions*)options
                          error: (NSError**)outError
                      withBlock: (CBForestValueIterator)block
{
    return [self _enumerateValuesFromKey: startKey toKey: endKey options: options error: outError
                               withBlock: ^BOOL(const fdb_doc *doc, uint64_t bodyOffset)
    {
        @autoreleasepool {
            NSData* key = BufToData(doc->key, doc->keylen);
            NSData* value = BufToData(doc->body, doc->bodylen);
            NSData* meta = BufToData(doc->meta, doc->metalen);
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
    if (![doc reloadMeta: outError])
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
    fdb_doc doc = {
        .seqnum = sequence
    };
    uint64_t bodyOffset = 0;
    if (!Check(fdb_get_metaonly_byseq(self.handle, &doc, &bodyOffset), outError))
        return nil;
    return [[_documentClass alloc] initWithDB: self info: &doc offset: bodyOffset];
}


#pragma mark - ITERATION:


- (BOOL) enumerateDocsFromID: (NSString*)startID
                        toID: (NSString*)endID
                     options: (const CBForestEnumerationOptions*)options
                       error: (NSError**)outError
                   withBlock: (CBForestDocIterator)block
{
    return [self _enumerateValuesFromKey: [startID dataUsingEncoding: NSUTF8StringEncoding]
                                   toKey: [endID dataUsingEncoding: NSUTF8StringEncoding]
                                 options: options
                                   error: outError
                               withBlock: ^BOOL(const fdb_doc *docinfo, uint64_t bodyOffset)
    {
        @autoreleasepool {
            if (![_documentClass docInfo: docinfo matchesOptions: options])
                return true;
            CBForestDocument* doc = [[_documentClass alloc] initWithDB: self
                                                                  info: docinfo
                                                                offset: bodyOffset];
            BOOL stop = NO;
            block(doc, &stop);
            return !stop;
        }
    }];
}


- (BOOL) enumerateDocsFromSequence: (CBForestSequence)startSequence
                        toSequence: (CBForestSequence)endSequence
                           options: (const CBForestEnumerationOptions*)options
                             error: (NSError**)outError
                         withBlock: (CBForestDocIterator)block
{
    // FIX: This hasn't been implemented in ForestDB, so we shall have to simulate it:

    if (options && !options->inclusiveEnd && endSequence > 0)
        endSequence--;
    if (startSequence > self.info.lastSequence || endSequence < startSequence)
        return YES; // no-op

    NSMutableArray* sequences = [[NSMutableArray alloc] init];
    BOOL ok = [self _enumerateValuesFromKey: nil toKey: nil options: options error: outError
                              withBlock: ^BOOL(const fdb_doc *docinfo, uint64_t bodyOffset)
    {
        fdb_seqnum_t sequence = docinfo->seqnum;
        if (sequence >= startSequence && sequence <= endSequence)
            [sequences addObject: @(sequence)];
        return true;
    }];
    if (!ok)
        return NO;

    [sequences sortUsingSelector: @selector(compare:)];

    CBForestContentOptions contentOptions = 0;
    if (options)
        contentOptions = options->contentOptions;
    for (NSNumber* sequence in sequences) {
        CBForestDocument* doc = [self documentWithSequence: sequence.unsignedLongLongValue
                                                   options: contentOptions error: outError];
        if (!doc)
            return NO;
        BOOL stop = NO;
        block(doc, &stop);
        if (stop)
            break;
    }
    return YES;
}


@end
