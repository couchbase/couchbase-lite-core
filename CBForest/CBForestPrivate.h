//
//  CBForestPrivate.h
//  CBForest
//
//  Created by Jens Alfke on 9/5/12.
//  Copyright (c) 2012 Couchbase, Inc. All rights reserved.
//

#import "CBForest.h"
#import "forestdb.h"
#import "slice.h"


// Declaring a function/method argument as __unsafe_unretained in the implementation stops ARC
// from retaining it at the start and releasing it at the end. ARC is being technically correct
// and conservative, but this really adds to the overhead of small performance-critical methods.
#define UU __unsafe_unretained


BOOL CheckFailed(fdb_status code, id key, NSError** outError);

static inline BOOL CheckWithKey(fdb_status code, id key, NSError** outError) {
    if (code == FDB_RESULT_SUCCESS) {
        if (outError)
            *outError = nil;
        return YES;
    } else {
        return CheckFailed(code, key, outError);
    }
}

static inline BOOL Check(fdb_status code, NSError** outError) {
    return CheckWithKey(code, nil, outError);
}

BOOL CBIsFileNotFoundError( NSError* error );


static inline slice DataToSlice(NSData* data) {
    return (slice){data.bytes, data.length};
}

static inline slice StringToSlice(NSString* string) {
    return DataToSlice([string dataUsingEncoding: NSUTF8StringEncoding]);
}

static inline NSData* SliceToData(const void* buf, size_t size) {
    if (!buf)
        return nil;
    return [NSData dataWithBytes: buf length: size];
}


static inline NSData* SliceToTempData(const void* buf, size_t size) {
    if (!buf)
        return nil;
    return [NSData dataWithBytesNoCopy: (void*)buf length: size freeWhenDone: NO];
}


static inline NSData* SliceToAdoptingData(const void* buf, size_t size) {
    if (!buf)
        return nil;
    return [NSData dataWithBytesNoCopy: (void*)buf length: size freeWhenDone: YES];
}


static inline NSString* SliceToString(const void* buf, size_t size) {
    if (!buf)
        return nil;
    return [[NSString alloc] initWithBytes: buf
                                    length: size
                                  encoding: NSUTF8StringEncoding];
}

NSData* JSONToData(id obj, NSError** outError) ;

static inline id DataToJSON(NSData* data, NSError** outError) {
    return [NSJSONSerialization JSONObjectWithData: data
                                           options: NSJSONReadingAllowFragments
                                             error: NULL];
}

id SliceToJSON(slice, NSError** outError);

void UpdateBuffer(void** outBuf, size_t *outLen, const void* srcBuf, size_t srcLen);
void UpdateBufferFromData(void** outBuf, size_t *outLen, NSData* data);

NSData* CompactRevID(NSString* revID);
NSString* ExpandRevID(slice compressedRevID);

static inline slice CompactRevIDToSlice(NSString* revID) {
    return DataToSlice(CompactRevID(revID));
}


// Calls the given block, passing it a UTF-8 encoded version of the given string.
// The UTF-8 data can be safely modified by the block but isn't valid after the block exits.
BOOL WithMutableUTF8(NSString* str, void (^block)(uint8_t*, size_t));


// The block is responsible for freeing the doc!
typedef BOOL (^CBForest_Iterator)(fdb_doc *doc, uint64_t bodyOffset);


@interface CBForestDB ()
- (BOOL) open: (NSString*)filePath
      options: (CBForestFileOptions)options
        error: (NSError**)outError;
- (void) beginTransaction;
- (BOOL) endTransaction: (NSError**)outError;
- (fdb_status) rawGet: (fdb_doc*)doc
              options: (CBForestContentOptions)options;
- (BOOL) rawSet: (fdb_doc*)doc error: (NSError**)outError;
- (NSEnumerator*) enumerateDocsFromKey: (NSData*)startKey
                                 toKey: (NSData*)endKey
                               options: (const CBForestEnumerationOptions*)options
                                 error: (NSError**)outError;
@end


@interface CBForestDocument ()
- (id) initWithDB: (CBForestDB*)store docID: (NSString*)docID;
- (id) initWithDB: (CBForestDB*)store
             info: (const fdb_doc*)info
          options: (CBForestContentOptions)options
            error: (NSError**)outError;
@property (readonly) slice rawID;
@property (readonly) slice rawMeta;
@property (readonly) fdb_doc* info;
@property (readonly) uint64_t fileOffset;
+ (BOOL) docInfo: (const fdb_doc*)docInfo
  matchesOptions: (const CBForestEnumerationOptions*)options;
@end


@interface CBForestIndex ()
- (BOOL) _updateForDocument: (NSString*)docID
                 atSequence: (CBForestSequence)docSequence
                    addKeys: (void(^)(CBForestIndexEmitBlock))addKeysBlock;
@end


NSUInteger CPUCount(void);


@interface CBForestToken : NSObject
@property (copy) NSString* name;
- (void) lockWithOwner: (id)owner;
- (void) unlockWithOwner: (id)oldOwner;
@end
