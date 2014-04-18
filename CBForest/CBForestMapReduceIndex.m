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
#import <forestdb.h>


@implementation CBForestMapReduceIndex
{
    NSString* _lastMapVersion;
}

@synthesize sourceDatabase=_sourceDatabase, map=_map, mapVersion=_mapVersion, lastSequenceIndexed=_lastSequenceIndexed;


- (id) initWithFile: (NSString*)filePath
            options: (CBForestFileOptions)options
              error: (NSError**)outError
{
    self = [super initWithFile: filePath options: options error: outError];
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
            _lastMapVersion = state[@"mapVersion"];
        }
    }
    return self;
}


- (BOOL) saveState: (NSError**)outError {
    NSMutableDictionary* state = [NSMutableDictionary dictionary];
    state[@"lastSequence"] = @(_lastSequenceIndexed);
    if (_mapVersion)
        state[@"mapVersion"] = _lastMapVersion;
    NSData* stateData = JSONToData(state, NULL);

    NSData* stateKey = CBCreateCollatable([NSNull null]);
    return [self setValue: stateData meta: nil forKey: stateKey error: outError];
}


- (void) setMapVersion:(NSString *)mapVersion {
    _mapVersion = mapVersion.copy;
    if (_lastMapVersion && ![_lastMapVersion isEqualToString: _mapVersion]) {
        [self erase: NULL];
        _lastSequenceIndexed = 0;
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

    CBForestSequence startSequence = _lastSequenceIndexed + 1;

    return [self inTransaction: ^BOOL {
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
            _lastSequenceIndexed = doc.sequence;
        }];

        _lastMapVersion = _mapVersion;
        if (_lastSequenceIndexed >= startSequence)
            ok = [self saveState: (gotError ?NULL :outError)];
        
        return ok && !gotError;
    }];
}


@end
