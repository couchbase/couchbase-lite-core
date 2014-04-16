//
//  CBForestMapReduceIndex.m
//  CBForest
//
//  Created by Jens Alfke on 4/4/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#import "CBForestMapReduceIndex.h"
#import "CBForestDocument.h"
#import "CBCollatable.h"
#import <forestdb.h>


@implementation CBForestMapReduceIndex
{
    CBForestSequence _nextSequence;
    BOOL _readNextSequence;
}

@synthesize sourceDatabase=_sourceDatabase, map=_map;


- (CBForestSequence) readNextSequence: (NSError**)outError {
    // (All keys in this db are in collatable form. So use 'null' for this key)
    NSData* nextSeqKey = CBCreateCollatable([NSNull null]);
    NSData* nextSeqData;
    if (![self getValue: &nextSeqData meta: NULL forKey: nextSeqKey error: outError])
        return UINT64_MAX;
    if (nextSeqData.length != sizeof(CBForestSequence))
        return 1;
    return NSSwapBigLongLongToHost(*(CBForestSequence*)nextSeqData.bytes);
}


- (BOOL) writeNextSequence: (CBForestSequence)nextSequence error: (NSError**)outError {
    NSData* nextSeqKey = CBCreateCollatable([NSNull null]);
    CBForestSequence bigEndian = NSSwapHostLongLongToBig(nextSequence);
    NSData* nextSeqData = [NSData dataWithBytes: &bigEndian length: sizeof(bigEndian)];
    return [self setValue: nextSeqData meta: nil forKey: nextSeqKey error: outError];
}


- (uint64_t) nextSequence: (NSError**)outError {
    if (!_readNextSequence) {
        _nextSequence = [self readNextSequence: outError];
        if (_nextSequence == UINT64_MAX)
            return 0;
        _readNextSequence = YES;
    }
    return _nextSequence;
}


- (uint64_t) lastSequenceIndexed {
    uint64_t next = [self nextSequence: NULL];
    return next > 0 ? next-1 : 0;
}


- (BOOL) updateIndex: (NSError**)outError {
    NSAssert(_sourceDatabase, @"No source database set!");
    NSAssert(_map, @"No map function set!");

    if ([self nextSequence: outError] == 0)
        return NO;

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

    CBForestSequence startSequence = _nextSequence;
    __block BOOL gotError = NO;
    BOOL ok = [_sourceDatabase enumerateDocsFromSequence: startSequence
                                              toSequence: kCBForestMaxSequence
                                                 options: NULL
                                                   error: outError
                                               withBlock: ^(CBForestDocument *doc, BOOL *stop)
    {
        [keys removeAllObjects];
        [values removeAllObjects];
        _map(doc, emit);
        if (![self setKeys: keys
                    values: values
               forDocument: doc.docID
                atSequence: doc.sequence
                     error: outError]) {
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
