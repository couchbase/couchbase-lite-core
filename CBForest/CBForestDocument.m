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


@synthesize db=_db, docID=_docID, bodyFileOffset=_bodyOffset;


- (id) initWithDB: (CBForestDB*)store docID: (NSString*)docID
{
    NSParameterAssert(store != nil);
    NSParameterAssert(docID != nil);
    self = [super init];
    if (self) {
        _db = store;
        _docID = [docID copy];
        sized_buf idbuf = CopyBuf(StringToBuf(docID));
        _info.keylen = idbuf.size;
        _info.key = idbuf.buf;
        _info.seqnum = kCBForestNoSequence;
    }
    return self;
}


- (id) initWithDB: (CBForestDB*)store
                info: (const fdb_doc*)info
              offset: (uint64_t)bodyOffset
{
    self = [super init];
    if (self) {
        _db = store;
        _info = *info;
        _docID = BufToString(_info.key, _info.keylen);
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


- (sized_buf) rawID                 {return (sized_buf){_info.key, _info.keylen};}
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

- (BOOL) reloadMeta: (NSError**)outError {
    uint64_t newBodyOffset;
    if (!Check(fdb_get_metaonly(_db.handle, &_info, &newBodyOffset),
               outError))
        return NO;
    _metadata = nil;
    _bodyOffset = newBodyOffset;
    return YES;
}


- (UInt64) bodyLength {
    return _info.bodylen;
}


- (NSData*) readBody: (NSError**)outError {
    if (_bodyOffset == 0) {
        if (![self reloadMeta: outError])
            return nil;
        assert(_bodyOffset > 0);
    }
    if (!Check(fdb_get_byoffset(_db.handle, &_info, _bodyOffset), outError))
        return nil;
    NSData* body = [NSData dataWithBytesNoCopy: _info.body length: _info.bodylen freeWhenDone: YES];
    _info.body = NULL;
    return body;
}


- (BOOL) writeBody: (NSData*)body metadata: (NSData*)metadata error: (NSError**)outError {
    fdb_doc newDoc = {
        .key = _info.key,
        .keylen = _info.keylen,
        .meta = (void*)metadata.bytes,
        .metalen = metadata.length,
        .body = (void*)body.bytes,
        .bodylen = body.length,
    };
    if (!Check(fdb_set(_db.handle, &newDoc), outError))
        return NO;
    _metadata = [metadata copy];
    free(_info.meta);
    _info.meta = NULL;
    _info.bodylen = newDoc.bodylen;
    _info.seqnum = newDoc.seqnum;
    _bodyOffset = 0; // don't know its new offset
    return YES;
}


- (BOOL) deleteDocument: (NSError**)outError {
    _info.body = NULL;
    _info.bodylen = 0;
    if (!Check(fdb_set(_db.handle, &_info), outError))
        return NO;
    _bodyOffset = 0;
    _info.seqnum = kCBForestNoSequence;
    return YES;
}


+ (BOOL) docInfo: (const fdb_doc*)docInfo matchesOptions: (const CBForestEnumerationOptions*)options {
    return YES;
}


@end
