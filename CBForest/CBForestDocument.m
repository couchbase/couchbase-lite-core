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
    CBForest* _db;
    fdb_doc _info;
    NSString* _docID;
    uint64_t _bodyOffset;
    BOOL _changed;
}


@synthesize db=_db, docID=_docID;


- (id) initWithStore: (CBForest*)store
               docID: (NSString*)docID
                info: (fdb_doc*)info
{
    NSParameterAssert(store != nil);
    NSParameterAssert(docID || info);
    self = [super init];
    if (self) {
        _db = store;
        if (info)
            _info = *info;
        if (docID)
            _docID = [docID copy];
        else
            _docID = BufToString(info->key, info->keylen);
        if (!_docID)
            return nil;
    }
    return self;
}


- (void)dealloc
{
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
- (uint64_t) dbSequence             {return _info.seqnum;}
- (BOOL) exists                     {return _info.seqnum > 0;}


- (BOOL) refreshMeta: (NSError**)outError {
    uint64_t bodyOffset;
    if (!Check(fdb_get_metaonly(_db.db, &_info, &bodyOffset),
               outError))
        return NO;
    return YES;
}


- (NSData*) loadData: (NSError**)outError {
    if (!_info.body) {
        if (!Check(fdb_get(_db.db, &_info), outError))
            return nil;
    }
    return self.data;
}


- (NSData*) data {
    return BufToData(_info.body, _info.bodylen);
}

- (void) setData:(NSData*) data {
    UpdateBuffer(&_info.body, &_info.bodylen, data);
    _changed = YES;
}


- (NSData*) metadata {
    return BufToData(_info.meta, _info.metalen);
}

- (void) setMetadata:(NSData*) metadata {
    UpdateBuffer(&_info.meta, &_info.metalen, metadata);
    _changed = YES;
}


- (BOOL) saveChanges: (NSError**)outError {
    if (!_changed)
        return YES;
    if (!Check(fdb_set(_db.db, &_info), outError))
        return NO;
    _changed = NO;
    return YES;
}


@end
