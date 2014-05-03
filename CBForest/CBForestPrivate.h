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


BOOL Check(fdb_status err, NSError** outError);

slice DataToSlice(NSData* data);
slice StringToSlice(NSString* docID);
NSData* SliceToData(const void* buf, size_t size);
NSData* SliceToTempData(const void* buf, size_t size);
NSData* SliceToAdoptingData(const void* buf, size_t size);
NSString* SliceToString(const void* buf, size_t size);

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
slice CompactRevIDToSlice(NSString* revID);
NSString* ExpandRevID(slice compressedRevID);


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
- (fdb_status) rawGetMeta: (fdb_doc*)doc offset: (uint64_t*)outOffset;
- (fdb_status) rawGetBody: (fdb_doc*)doc byOffset: (uint64_t)offset;
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
           offset: (uint64_t)bodyOffset
          options: (CBForestContentOptions)options
            error: (NSError**)outError;
@property (readonly) slice rawID;
@property (readonly) slice rawMeta;
@property (readonly) fdb_doc* info;
@property (readonly) uint64_t bodyFileOffset;
+ (BOOL) docInfo: (const fdb_doc*)docInfo
  matchesOptions: (const CBForestEnumerationOptions*)options;
@end


NSUInteger CPUCount(void);


@interface CBForestToken : NSObject
@property (copy) NSString* name;
- (void) lockWithOwner: (id)owner;
- (void) unlockWithOwner: (id)oldOwner;
@end
