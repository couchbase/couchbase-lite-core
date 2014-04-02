//
//  CBForestDocument.m
//  CBForest
//
//  Created by Jens Alfke on 9/5/12.
//  Copyright (c) 2012 Couchbase, Inc. All rights reserved.
//

#import "CBForestDocument.h"
#import "CBForestPrivate.h"
#import "forestdb_x.h"


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
- (uint64_t) sequence               {return _info.seqnum;}
- (BOOL) exists                     {return _info.seqnum != kForestDocNoSequence;}


- (BOOL) reloadMeta: (NSError**)outError {
    uint64_t newBodyOffset;
    if (!Check(fdb_get_metaonly(_db.db, &_info, &newBodyOffset),
               outError))
        return NO;
    if (newBodyOffset != _bodyOffset) {
        _bodyOffset = newBodyOffset;
        free(_info.body);
        _info.body = NULL;
    }
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


- (void) unloadBody {
    free(_info.body);
    _info.body = NULL;
}


- (NSData*) getBody: (NSError**)outError {
    if (!_info.body) {
        if (_bodyOffset > 0) {
            if (!Check(x_fdb_read_body(_db.db, &_info, _bodyOffset), outError))
                return nil;
        } else {
            if (![self reload: outError])
                return nil;
        }
    }
    return self.body;
}


- (BOOL) reload: (NSError**)outError {
    // We don't call fdb_get, because it doesn't tell us the bodyOffset.
    // Instead, load the metadata first, then the body.
    return [self reloadMeta: outError]
        && Check(x_fdb_read_body(_db.db, &_info, _bodyOffset), outError);
}


+ (CBForestDocumentFlags) flagsFromMeta: (const fdb_doc*)docinfo {
    if (docinfo->metalen == 0)
        return 0;
    return ((UInt8*)docinfo->meta)[0];
}

- (CBForestDocumentFlags)flags {
    if (_info.metalen == 0)
        return 0;
    return ((UInt8*)_info.meta)[0];
}

- (void)setFlags: (CBForestDocumentFlags)flags {
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
    return ExpandRevID((sized_buf){&((UInt8*)_info.meta)[1], _info.metalen - 1});
}

- (void) setRevID: (NSString*)revID {
    NSData* revIDData = CompactRevID(revID);
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
        if (![self getBody: outError])
            return NO;
    }
    if (!Check(fdb_set(_db.db, &_info), outError))
        return NO;
    _bodyOffset = 0; // don't know its new offset
    _changed = NO;
    return YES;
}


- (BOOL) deleteDocument: (NSError**)outError {
    free(_info.body);
    _info.body = NULL;
    _info.bodylen = 0;
    if (!Check(fdb_set(_db.db, &_info), outError))
        return NO;
    _changed = NO;
    _bodyOffset = 0;
    _info.seqnum = kForestDocNoSequence;
    return YES;
}


@end
