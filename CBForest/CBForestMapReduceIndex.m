//
//  CBForestMapReduceIndex.m
//  CBForest
//
//  Created by Jens Alfke on 4/4/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#import "CBForestMapReduceIndex.h"
#import "CBForestDocument.h"
#import "CBForestPrivate.h"
#import "CBCollatable.h"
#import "CBTextTokenizer.h"
#import <forestdb.h>


#define MAP_PARALLELISM 8 /* If defined, min number of GCD tasks to use for map functions */


@implementation CBForestMapReduceIndex
{
    NSString* _lastMapVersion;
}

@synthesize sourceDatabase=_sourceDatabase, map=_map, mapVersion=_mapVersion, indexType=_indexType,
            textTokenizer=_textTokenizer,
            lastSequenceIndexed=_lastSequenceIndexed, lastSequenceChangedAt=_lastSequenceChangedAt;


- (id) initWithFile: (NSString*)filePath
            options: (CBForestFileOptions)options
             config: (const CBForestDBConfig*)config
              error: (NSError**)outError
{
    self = [super initWithFile: filePath options: options config: config error: outError];
    if (self) {
        // Read the last-sequence and version properties.
        // (All keys in this db are in collatable form. So use 'null' for this key)
        NSData* stateKey = CBCreateCollatable([NSNull null]);
        NSData* stateData;
        if (![self getValue: &stateData meta: NULL forKey: stateKey error: outError])
            return nil;
        if (stateData) {
            NSDictionary* state = DataToJSON(stateData, NULL);
            _lastSequenceIndexed = [state[@"lastSequence"] unsignedLongLongValue];
            _lastSequenceChangedAt = [state[@"changedSequence"] unsignedLongLongValue];
            _lastMapVersion = state[@"mapVersion"];
            _indexType = [state[@"type"] intValue];
        }
    }
    return self;
}


- (BOOL) saveState: (NSError**)outError {
    NSMutableDictionary* state = [NSMutableDictionary dictionary];
    state[@"lastSequence"] = @(_lastSequenceIndexed);
    state[@"changedSequence"] = @(_lastSequenceChangedAt);
    state[@"type"] = @(_indexType);
    if (_mapVersion)
        state[@"mapVersion"] = _lastMapVersion;
    NSData* stateData = JSONToData(state, NULL);

    NSData* stateKey = CBCreateCollatable([NSNull null]);
    return [self setValue: stateData meta: nil forKey: stateKey error: outError] != kCBForestNoSequence;
}


- (BOOL) erase:(NSError *__autoreleasing *)outError {
    _lastSequenceIndexed = 0;
    _lastSequenceChangedAt = 0;
    return [super erase: outError];
}


- (void) setIndexType:(int)indexType {
    if (indexType != _indexType) {
        _indexType = indexType;
        [self erase: NULL];
        [self saveState: NULL];
    }
}


- (void) setMapVersion:(NSString *)mapVersion {
    _mapVersion = mapVersion.copy;
    if (_lastMapVersion && ![_lastMapVersion isEqualToString: _mapVersion]) {
        [self erase: NULL];
        _lastMapVersion = nil;
    }
}


- (BOOL) updateIndex: (NSError**)outError {
    NSAssert(_sourceDatabase, @"No source database set!");
    NSAssert(_map, @"No map function set!");
    NSAssert(_mapVersion, @"No map version set!");

    // If the map function has changed, erase the index and start over:
    if (_lastMapVersion && ![_lastMapVersion isEqualToString: _mapVersion]) {
        if (![self erase: outError])
            return NO;
        _lastSequenceIndexed = 0;
        _lastMapVersion = nil;
    }

    CBForestSequence startSequence = _lastSequenceIndexed + 1;

    CBForestEnumerationOptions options = {
        .includeDeleted = YES, // we need to scan deleted docs to remove any old emitted rows
    };

    CBTextTokenizer* tokenizer = _textTokenizer;

#ifdef MAP_PARALLELISM
    dispatch_semaphore_t mapCounter = dispatch_semaphore_create(MAX(MAP_PARALLELISM, CPUCount()/2));
    dispatch_group_t mapGroup = dispatch_group_create();
    dispatch_queue_t mapQueue = dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0);
#endif
    __block BOOL gotError = NO;
    [self inTransaction: ^BOOL {
        NSEnumerator* e = [_sourceDatabase enumerateDocsFromSequence: startSequence
                                                          toSequence: kCBForestMaxSequence
                                                             options: &options
                                                               error: outError];
        if (!e)
            return NO;
        __block uint32_t indexChanged = 0;
        while(true) {
            @autoreleasepool {
                CBForestDocument* doc = e.nextObject;
                if (!doc)
                    break;
                CBForestSequence sequence = doc.sequence;
                NSData* body = [doc readBody: NULL];
#ifdef MAP_PARALLELISM
                dispatch_semaphore_wait(mapCounter, DISPATCH_TIME_FOREVER);
                dispatch_group_async(mapGroup, mapQueue, ^{
                    @autoreleasepool {
#endif
                        BOOL changed = [self _updateForDocument: doc.docID atSequence: sequence
                                                        addKeys: ^(CBForestIndexEmitBlock emit)
                        {
                            if (tokenizer) {
                                // Full-text indexing:
                                CBForestIndexEmitBlock realEmit = emit;
                                emit = ^(id key, id value) {
                                    if ([key isKindOfClass: [NSString class]]) {
                                        for (NSString* word in [tokenizer tokenize: key])
                                            realEmit(word, value);
                                    } else {
                                        realEmit(key, value);
                                    }
                                };
                            }
                            @try {
                                _map(doc, body, emit);
                            } @catch (NSException* x) {
                                NSLog(@"WARNING: CBForestMapReduceIndex caught exception from map fn: %@", x);
                            }
                        }];
                        if (changed)
                            OSAtomicOr32(1, &indexChanged);
#ifdef MAP_PARALLELISM
                    }
                    dispatch_semaphore_signal(mapCounter);
                });
#endif
                _lastSequenceIndexed = sequence;
            }
        }

    #ifdef MAP_PARALLELISM
        // Wait for all the blocks to be processed on both the mapQueue and updateQueue:
        dispatch_group_wait(mapGroup, DISPATCH_TIME_FOREVER);
    #endif

        _lastMapVersion = _mapVersion;
        if (_lastSequenceIndexed >= startSequence) {
            if (indexChanged)
                _lastSequenceChangedAt = _lastSequenceIndexed;
            if (![self saveState: (gotError ? NULL : outError)])
                gotError = YES;
        }
        return YES;
    }];
    [_textTokenizer clearCache];
    return !gotError;
}


- (NSEnumerator*) enumerateDocsContainingWords: (NSString*)words
                                           all: (BOOL)all
                                         error: (NSError**)outError
{
    CBTextTokenizer* tok = [[CBTextTokenizer alloc] init];
    tok.tokenCharacters = @"*";
    NSSet* keys = [tok tokenize: words];
    return [[CBForestQueryMultiKeyEnumerator alloc] initWithIndex: self
                                                             keys: keys.allObjects
                                                     intersection: all
                                                            error: outError];
}



@end
