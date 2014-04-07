//
//  CBForestMapReduceIndex.m
//  CBForest
//
//  Created by Jens Alfke on 4/4/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#import "CBForestMapReduceIndex.h"
#import "CBForestDocument.h"
#import <forestdb.h>


@implementation CBForestMapReduceIndex
{
    uint64_t _nextSequence;
    BOOL _readNextSequence;
}

@synthesize sourceDatabase=_sourceDatabase, map=_map;


- (uint64_t) readNextSequence: (NSError**)outError {
    // (The custom compare fn means all keys in this db have to be JSON. So use 'null' for this key)
    NSData* nextSeqKey = [@"null" dataUsingEncoding: NSUTF8StringEncoding];
    NSData* nextSeqData;
    if (![self getValue: &nextSeqData meta: NULL forKey: nextSeqKey error: outError])
        return SEQNUM_NOT_USED;
    if (nextSeqData.length != sizeof(uint64_t))
        return 0;
    return NSSwapBigLongLongToHost(*(uint64_t*)nextSeqData.bytes);
}


- (BOOL) writeNextSequence: (uint64_t)nextSequence error: (NSError**)outError {
    NSData* nextSeqKey = [@"null" dataUsingEncoding: NSUTF8StringEncoding];
    uint64_t bigEndian = NSSwapHostLongLongToBig(nextSequence);
    NSData* nextSeqData = [NSData dataWithBytes: &bigEndian length: sizeof(bigEndian)];
    return [self setValue: nextSeqData meta: nil forKey: nextSeqKey error: outError];
}


- (BOOL) updateIndex: (NSError**)outError {
    NSAssert(_sourceDatabase, @"No source database set!");
    NSAssert(_map, @"No map function set!");
    if (!_readNextSequence) {
        _nextSequence = [self readNextSequence: outError];
        if (_nextSequence == SEQNUM_NOT_USED)
            return NO;
        _readNextSequence = YES;
    }

    NSMutableArray* keys = [NSMutableArray array];
    NSMutableArray* values = [NSMutableArray array];

    CBForestIndexEmitBlock emit = ^(id key, id value) {
        @try {
            [keys addObject: key];
            [values addObject: value ?: kCBForestIndexNoValue];
        } @catch (NSException* x) {
            NSLog(@"WARNING: CBForestMapReduceIndex caught exception from map fn: %@", x);
        }
    };

    uint64_t startSequence = _nextSequence;
    __block BOOL gotError = NO;
    BOOL ok = [_sourceDatabase enumerateDocsFromSequence: startSequence
                                        toSequence: SEQNUM_NOT_USED
                                           options: NULL
                                             error: outError
                                         withBlock: ^(CBForestDocument *doc, BOOL *stop)
    {
        [keys removeAllObjects];
        [values removeAllObjects];
        _map(doc, emit);
        if (![self setKeys: keys values: values forDocument: doc.docID error: outError]) {
            *stop = gotError = YES;
            return;
        }
        _nextSequence = doc.sequence + 1;
    }];

    if (_nextSequence > startSequence)
        ok = [self writeNextSequence: _nextSequence error: (gotError ?NULL :outError)];
    
    if (!ok || gotError)
        return NO;
    return [self commit: outError];
}


@end
