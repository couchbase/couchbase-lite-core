//
//  CBForestDocument.m
//  CBForest
//
//  Created by Jens Alfke on 9/5/12.
//  Copyright (c) 2012 Couchbase, Inc. All rights reserved.
//

#import "CBForestDocument.h"
#import "CBForestPrivate.h"


@implementation CBForestDocument
{
    CBForestDB* _db;
    fdb_doc _info;
    NSString* _docID;
    NSData* _metadata;
    uint64_t _bodyOffset;
}


@synthesize db=_db, bodyFileOffset=_bodyOffset;


- (id) initWithDB: (CBForestDB*)store docID: (NSString*)docID
{
    NSParameterAssert(store != nil);
    NSParameterAssert(docID != nil);
    self = [super init];
    if (self) {
        _db = store;
        _docID = [docID copy];
        slice idbuf = slicecopy(StringToSlice(docID));
        _info.keylen = idbuf.size;
        _info.key = (void*)idbuf.buf;
        _info.seqnum = kCBForestNoSequence;
    }
    return self;
}


- (id) initWithDB: (CBForestDB*)store
             info: (const fdb_doc*)info
           offset: (uint64_t)bodyOffset
          options: (CBForestContentOptions)options
            error: (NSError**)outError
{
    self = [super init];
    if (self) {
        _db = store;
        _info = *info;
        _bodyOffset = bodyOffset;
    }
    return self;
}


- (void)dealloc {
    free(_info.key);
    free(_info.body);
    free(_info.meta);
}


- (NSString*) description {
    return [NSString stringWithFormat: @"%@[%@]", [self class], _docID];
}


- (BOOL) isEqual: (id)object {
    return [object isKindOfClass: [CBForestDocument class]]
        && _db == [object db]
        && [_docID isEqualToString: [object docID]];
}

- (NSUInteger) hash {
    return _docID.hash;
}


- (NSString*) docID {
    if (!_docID)
        _docID = SliceToString(_info.key, _info.keylen);
    return _docID;
}


- (slice) rawID                     {return (slice){_info.key, _info.keylen};}
- (slice) rawMeta                   {return (slice){_info.meta, _info.metalen};}
- (fdb_doc*) info                   {return &_info;}
- (CBForestSequence) sequence       {return _info.seqnum;}
- (BOOL) exists                     {return _info.seqnum != kCBForestNoSequence;}


- (NSData*) metadata {
    if (!_metadata && _info.meta != NULL) {
        _metadata = [NSData dataWithBytesNoCopy: _info.meta length: _info.metalen
                                   freeWhenDone: YES];
        _info.meta = NULL;
    }
    return _metadata;
}

- (BOOL) reload: (CBForestContentOptions)options error: (NSError **)outError {
    uint64_t newBodyOffset = 0;
    fdb_status status;
    if (options & kCBForestDBMetaOnly)
        status = [_db rawGetMeta: &_info offset: &newBodyOffset];
    else
        status = [_db rawGetBody: &_info byOffset: 0];
    if (status == FDB_RESULT_KEY_NOT_FOUND) {
        _info.seqnum = kCBForestNoSequence;
        newBodyOffset = 0;
    } else if (!Check(status, outError)) {
        return NO;
    }
    _metadata = nil; // forget old cached metadata
    _bodyOffset = newBodyOffset;
    return YES;
}


- (UInt64) bodyLength {
    return _info.bodylen;
}


- (NSData*) readBody: (NSError**)outError {
    if (_info.body == NULL) {
        if (_bodyOffset == 0) {
            if (![self reload: 0 error: outError])
                return nil;
            _metadata = nil;
        } else {
            if (!Check([_db rawGetBody: &_info byOffset: _bodyOffset], outError))
                return nil;
        }
    }
    // Rather than copying the body data, just let the NSData adopt it and clear the local ptr.
    // This assumes that the typical client will only read the body once.
    NSData* body = SliceToAdoptingData(_info.body, _info.bodylen);
    _info.body = NULL;
    return body;
}


- (BOOL) writeBody: (NSData*)body metadata: (NSData*)metadata error: (NSError**)outError {
    return [_db inTransaction: ^BOOL{
        fdb_doc newDoc = {
            .key = _info.key,
            .keylen = _info.keylen,
            .meta = (void*)metadata.bytes,
            .metalen = metadata.length,
            .body = (void*)body.bytes,
            .bodylen = body.length,
        };
        if (![_db rawSet: &newDoc error: outError])
            return NO;
        _metadata = [metadata copy];
        free(_info.meta);
        _info.meta = NULL;
        _info.body = NULL;
        _info.bodylen = newDoc.bodylen;
        _info.seqnum = newDoc.seqnum;
        _bodyOffset = 0; // don't know its new offset
        return YES;
    }];
}


- (BOOL) deleteDocument: (NSError**)outError {
    _info.body = NULL;
    _info.bodylen = 0;
    if (![_db rawSet: &_info error: outError])
        return NO;
    _bodyOffset = 0;
    _info.seqnum = kCBForestNoSequence;
    return YES;
}


+ (BOOL) docInfo: (const fdb_doc*)docInfo matchesOptions: (const CBForestEnumerationOptions*)options {
    return YES;
}


@end
