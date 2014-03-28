//
//  CBForestDocument.m
//  CBForest
//
//  Created by Jens Alfke on 9/5/12.
//  Copyright (c) 2012 Couchbase, Inc. All rights reserved.
//

#import "CBForestDocument.h"
#import "CBForestPrivate.h"


const UInt64 kForestDocNoSequence = SEQNUM_NOT_USED;


@implementation CBForestDocument
{
    CBForestDB* _db;
    fdb_doc _info;
    NSString* _docID;
    uint64_t _bodyOffset; // this isn't really useable yet as not all the API calls set it
}


@synthesize db=_db, docID=_docID, bodyFileOffset=_bodyOffset, changed=_changed;


- (id) initWithStore: (CBForestDB*)store
               docID: (NSString*)docID
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
        _info.seqnum = kForestDocNoSequence;
    }
    return self;
}


- (id) initWithStore: (CBForestDB*)store
                info: (fdb_doc*)info
{
    self = [super init];
    if (self) {
        _db = store;
        _info = *info;
        _docID = BufToString(_info.key, _info.keylen);
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
- (uint64_t) sequence               {return _info.seqnum;}
- (BOOL) exists                     {return _info.seqnum != kForestDocNoSequence;}


- (BOOL) reloadMeta: (NSError**)outError {
    if (!Check(fdb_get_metaonly(_db.db, &_info, &_bodyOffset),
               outError))
        return NO;
    free(_info.body);       //FIX: Should do this only if a newer version was read
    _info.body = NULL;
    return YES;
}

- (UInt64) bodyLength {
    return _info.bodylen;
}

- (NSData*) body {
    return BufToData(_info.body, _info.bodylen);
}

- (void) setBody:(NSData*) data {
    [self setBodyBytes: data.bytes length: data.length noCopy: NO];
}

- (void) setBodyBytes: (const void*)bytes length: (size_t)length noCopy: (BOOL)noCopy {
    NSParameterAssert(bytes!=NULL || length==0);
    if (noCopy) {
        _info.body = (void*)bytes;
        _info.bodylen = length;
    } else {
        UpdateBuffer(&_info.body, &_info.bodylen, bytes, length);
    }
    _changed = YES;
}


- (NSData*) getBody: (NSError**)outError {
    if (!_info.body && ![self reload: outError])
        return nil;
    return self.body;
}


- (BOOL) reload: (NSError**)outError {
    if (!Check(fdb_get(_db.db, &_info), outError))
        return NO;
    _changed = NO;
    return YES;
}


- (CBForestDocumentFlags)flags {
    if (_info.metalen == 0)
        return 0;
    return ((UInt8*)_info.meta)[0];
}

- (void)setFlags:(CBForestDocumentFlags)flags {
    if (_info.metalen == 0) {
        _info.meta = malloc(1);
        _info.metalen = 1;
    }
    if (flags != ((UInt8*)_info.meta)[0]) {
        ((UInt8*)_info.meta)[0] = flags;
        _changed = YES;
    }
}


- (NSString*) revID {
    if (_info.metalen <= 1)
        return nil;
    return [[NSString alloc] initWithBytes: &((UInt8*)_info.meta)[1]
                                    length: _info.metalen - 1
                                  encoding: NSUTF8StringEncoding];
}

- (void) setRevID: (NSString*)revID {
    NSData* revIDData = [revID dataUsingEncoding: NSUTF8StringEncoding];
    size_t newMetaLen = 1 + revIDData.length;
    if (newMetaLen != _info.metalen) {
        CBForestDocumentFlags flags = self.flags;
        free(_info.meta);
        _info.meta = malloc(newMetaLen);
        _info.metalen = newMetaLen;
        self.flags = flags;
    }
    memcpy(&((UInt8*)_info.meta)[1], revIDData.bytes, revIDData.length);
    _changed = YES;
}


- (BOOL) saveChanges: (NSError**)outError {
    if (!_changed)
        return YES;
    if (!_info.body) {
        // If the body hasn't been loaded, we need to load it now otherwise fdb_set will
        // reset it to empty.
        if (!Check(fdb_get(_db.db, &_info), outError))
            return NO;
    }
    if (!Check(fdb_set(_db.db, &_info), outError))
        return NO;
    _changed = NO;
    return YES;
}


@end
