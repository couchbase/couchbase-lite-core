//
//  CBForestIndex.m
//  CBForest
//
//  Created by Jens Alfke on 4/1/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#import "CBForestIndex.h"
#import "CBForestPrivate.h"
#import "CBCollatable.h"
#import "varint.h"
#import <forestdb.h>


id kCBForestIndexNoValue;


static BOOL parseKey(CBForestDocument* doc,
                     id* key, NSString** docID, CBForestSequence* sequence);


@implementation CBForestIndex


+ (void) initialize {
    if (!kCBForestIndexNoValue)
        kCBForestIndexNoValue = [[NSObject alloc] init];
}


- (BOOL) _removeOldRowsForDoc: (NSData*)collatebleDocID {
    NSData* oldSeqData;
    if (![self getValue: &oldSeqData meta: NULL forKey: collatebleDocID error: NULL])
        return NO;
    if (!oldSeqData.length)
        return NO;
    // Decode a series of sequences from packed varint data:
    slice seqBuf = DataToSlice(oldSeqData);
    uint64_t seq;
    while (ReadUVarInt(&seqBuf, &seq)) {
        // ...and delete the old key/values with those sequences:
        [self deleteSequence: seq error: NULL];
    }
    return YES;
}


- (BOOL) updateForDocument: (NSString*)docID
                atSequence: (CBForestSequence)docSequence
                   addKeys: (void(^)(CBForestIndexEmitBlock))addKeysBlock
{
    __block BOOL updated;
    [self inTransaction: ^BOOL{
        updated = [self _updateForDocument: docID atSequence: docSequence addKeys: addKeysBlock];
        return YES;
    }];
    return updated;
}

- (BOOL) _updateForDocument: (NSString*)docID
                 atSequence: (CBForestSequence)docSequence
                    addKeys: (void(^)(CBForestIndexEmitBlock))addKeysBlock
{
    NSData* collatableDocID = CBCreateCollatable(docID);
    BOOL hadRows = [self _removeOldRowsForDoc: collatableDocID];

    __block NSMutableData* seqs = nil;
    __block int32_t rowsAdded = 0;
    __block int32_t rowsPending = 0;

    CBForestIndexEmitBlock emit = ^(id key, id value) {
        @autoreleasepool {
            if (!key)
                return;
            NSMutableData* keyData = [NSMutableData dataWithCapacity: 1024];
            CBCollatableBeginArray(keyData);
            CBAddCollatable(key, keyData);
            [keyData appendData: collatableDocID];
            CBAddCollatable(@(docSequence), keyData);
            CBCollatableEndArray(keyData);

            NSData* bodyData;
            if (value) {
                bodyData = JSONToData(value, NULL);
                if (!bodyData) {
                    NSLog(@"WARNING: Can't index non-JSON value %@", value);
                    return;
                }
            } else {
                bodyData = [NSData data];
            }

            ++rowsAdded;
            OSAtomicIncrement32(&rowsPending);
            [self asyncSetValue: bodyData
                           meta: nil
                         forKey: keyData
                     onComplete:^(CBForestSequence seq, NSError *error)
            {
                if (seq != kCBForestNoSequence) {
                    if (!seqs)
                        seqs = [[NSMutableData alloc] initWithCapacity: 200];
                    uint8_t buf[kMaxVarintLen64];
                    size_t size = PutUVarInt(buf, seq);
                    [seqs appendBytes: buf length: size];
                }
                if (OSAtomicDecrement32(&rowsPending) == 0) {
                    // Last set is done! Update the list of sequences used for this document:
                    [self asyncSetValue: seqs
                                   meta: nil
                                 forKey: collatableDocID
                             onComplete: nil];
                }
            }];
        }
    };

    addKeysBlock(emit);

    // If this call didn't alter the index, return NO:
    return hadRows || rowsAdded;
}


- (NSString*) dump {
    NSMutableString* dump = [NSMutableString stringWithCapacity: 1000];
    NSEnumerator* e = [self enumerateDocsFromID: nil toID: nil options: 0 error: NULL];
    for (CBForestDocument* doc in e) {
        id key;
        NSString *docID;
        CBForestSequence sequence;
        if (!parseKey(doc, &key, &docID, &sequence))
            continue;
        NSData* json = JSONToData(key, NULL);
        NSString* keyStr = [[NSString alloc] initWithData: json encoding: NSUTF8StringEncoding];
        json = [doc readBody: NULL];
        NSString* valueStr = @"";
        if (json)
            valueStr = [[NSString alloc] initWithData: json encoding: NSUTF8StringEncoding];
        [dump appendFormat: @"\t%@ -> %@ (doc \"%@\", seq %llu)\n",
                             keyStr, valueStr, docID, sequence];
    }
    return dump;
}


@end



@implementation CBForestQueryEnumerator
{
    CBForestIndex* _index;
    CBForestEnumerationOptions _options;
    NSEnumerator* _indexEnum;
    id _stopBeforeKey;
    NSEnumerator* _keys;
    NSData* _valueData;
    id _value;
}

@synthesize key=_key, valueData=_valueData, docID=_docID, sequence=_sequence, error=_error;


- (instancetype) initWithIndex: (CBForestIndex*)index
                      startKey: (id)startKey
                    startDocID: (NSString*)startDocID
                        endKey: (id)endKey
                      endDocID: (NSString*)endDocID
                       options: (const CBForestEnumerationOptions*)options
                         error: (NSError**)outError
{
    self = [super init];
    if (self) {
        _index = index;
        _options = options ? *options : kCBForestEnumerationOptionsDefault;

        // Remember, the underlying keys are of the form [emittedKey, docID, serial#]
        NSMutableArray* realStartKey = [NSMutableArray arrayWithObjects: startKey, startDocID, nil];
        NSMutableArray* realEndKey = [NSMutableArray arrayWithObjects: endKey, endDocID, nil];
        NSMutableArray* maxKey = (options && options->descending) ? realStartKey : realEndKey;
        [maxKey addObject: @{}];

        _stopBeforeKey = (options && !options->inclusiveEnd) ? endKey : nil;

        if (![self iterateFromKey: realStartKey toKey: realEndKey error: outError])
            return nil;
    }
    return self;
}


- (instancetype) initWithIndex: (CBForestIndex*)index
                          keys: (NSEnumerator*)keys
                       options: (const CBForestEnumerationOptions*)options
                         error: (NSError**)outError
{
    self = [super init];
    if (self) {
        _index = index;
        _options = options ? *options : kCBForestEnumerationOptionsDefault;
        _keys = keys;
        if (![self nextKey]) {
            if (outError)
                *outError = _error;
            return nil;
        }
    }
    return self;
}


- (BOOL) iterateFromKey: (id)realStartKey toKey: (id)realEndKey error: (NSError**)outError {
    _indexEnum = [_index enumerateDocsFromKey: CBCreateCollatable(realStartKey)
                                        toKey: CBCreateCollatable(realEndKey)
                                      options: &_options
                                        error: outError];
    return (_indexEnum != nil);
}


// go on to the next key in the array
- (BOOL) nextKey {
    id key = _keys.nextObject;
    if (!key)
        return NO;
    NSError* error;
    if (![self iterateFromKey: @[key] toKey: @[key, @{}] error: &error]) {
        _error = error;
        return NO;
    }
    return YES;
}


- (id) nextObject {
    _error = nil;

    CBForestDocument* doc;
    do {
        doc = _indexEnum.nextObject;
        if (!doc && ![self nextKey])
            return nil;
    } while (!doc);

    // Decode the key from collatable form:
    id key;
    NSString* docID;
    CBForestSequence docSequence;
    parseKey(doc, &key, &docID, &docSequence);
    NSAssert(key && docID, @"Bogus view key");

    if ([_stopBeforeKey isEqual: key])
        return nil;

    // Decode the value:
    NSData* valueData = nil;
    if (doc.bodyLength > 0) {
        NSError* error;
        valueData = [doc readBody: &error];
        if (!valueData) {
            _error = error;
            return nil;
        }
    }

    _key = key;
    _docID = docID;
    _value = nil;
    _valueData = valueData;
    _sequence = docSequence;
    return _key;
}


- (id) value {
    if (!_value && _valueData) {
        _value = [NSJSONSerialization JSONObjectWithData: _valueData
                                                 options: NSJSONReadingAllowFragments
                                                   error: NULL];
    }
    return _value;
}

@end



static NSString* nextObjectDocID(NSEnumerator* e) {
    CBForestDocument* doc = e.nextObject;
    if (!doc)
        return nil;
    id key;
    NSString* docID;
    CBForestSequence docSequence;
    parseKey(doc, &key, &docID, &docSequence);
    return docID;
}



@implementation CBForestQueryMultiKeyEnumerator
{
    BOOL _intersection;
    NSMutableArray* _enumerators;
    NSMutableArray* _curDocIDs;
}

- (instancetype) initWithIndex: (CBForestIndex*)index
                          keys: (NSArray*)keys
                  intersection: (BOOL)intersection
                         error: (NSError**)outError
{
    self = [super init];
    if (self) {
        _intersection = intersection;
        _curDocIDs = [NSMutableArray arrayWithCapacity: keys.count];
        _enumerators = [NSMutableArray arrayWithCapacity: keys.count];
        // Build the parallel arrays of enumerators and their current docIDs:
        for (NSString* key in keys) {
            // Remember, the underlying keys are of the form [emittedKey, docID, serial#]
            NSString *key1 = key, *key2 = key;
            if ([key hasSuffix: @"*"]) {
                key1 = [key substringToIndex: key.length-1];
                key2 = [key1 stringByAppendingString: @"\uFFFE"];//FIX
            }
            NSEnumerator* e = [index enumerateDocsFromKey: CBCreateCollatable(@[key1])
                                                    toKey: CBCreateCollatable(@[key2, @{}])
                                                  options: NULL
                                                    error: outError];
            if (!e)
                return nil;
            NSString* docID = nextObjectDocID(e);
            if (docID) {
                [_curDocIDs addObject: docID];
                [_enumerators addObject: e];
            } else if (intersection) {
                _curDocIDs = _enumerators = nil;
                break;
            }
        }
    }
    return self;
}

- (id) nextObject {
    if (_curDocIDs.count == 0)
        return nil;

    BOOL allEqual;
    NSString* minDocID;
    do {
        // Find the minimum docID of any of the enumerators:
        //FIX: -compare: doesn't use the same ordering as the collatable doc IDs in the index.
        minDocID = nil;
        for (NSString* docID in _curDocIDs)
            if (!minDocID || [docID compare: minDocID] < 0)
                minDocID = docID;

        // Now advance all the iterator(s) that have that docID:
        allEqual = YES;
        BOOL reachedEnd = NO;
        for (NSInteger i = _curDocIDs.count - 1; i >= 0; i--) {
            if ([minDocID isEqualToString: _curDocIDs[i]]) {
                NSString* docID;
                do {
                    docID = nextObjectDocID(_enumerators[i]);
                } while ([minDocID isEqualToString: docID]);
                if (docID) {
                    _curDocIDs[i] = docID;
                } else {
                    reachedEnd = YES;
                    [_curDocIDs removeObjectAtIndex: i];
                    [_enumerators removeObjectAtIndex: i];
                }
            } else {
                allEqual = NO;
            }
        }
        if (reachedEnd && _intersection) {
            _curDocIDs = _enumerators = nil;
        }
    } while (_intersection && !allEqual);
    return minDocID;
}

@end




static BOOL parseKey(CBForestDocument* doc,
                     id* key, NSString** docID, CBForestSequence* sequence)
{
    slice indexKey = doc.rawID;
    return CBCollatableReadNext(&indexKey, NO, key) == kArrayType
        && CBCollatableReadNext(&indexKey, YES, key) != kErrorType
        && CBCollatableReadNext(&indexKey, NO, docID) == kStringType
        && CBCollatableReadNextNumber(&indexKey, (int64_t*)sequence);
}
