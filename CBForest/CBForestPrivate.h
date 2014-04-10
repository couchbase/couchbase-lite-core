//
//  CBForestPrivate.h
//  CBForest
//
//  Created by Jens Alfke on 9/5/12.
//  Copyright (c) 2012 Couchbase, Inc. All rights reserved.
//

#import "CBForestDB.h"
#import "CBForestDocument.h"
#import "CBForestVersions.h"
#import "forestdb.h"

#import "rev_tree.h"


BOOL Check(fdb_status err, NSError** outError);

sized_buf DataToBuf(NSData* data);
sized_buf StringToBuf(NSString* docID);
NSData* BufToData(const void* buf, size_t size);
NSString* BufToString(const void* buf, size_t size);

sized_buf CopyBuf(sized_buf buf);

int CompareBufs(sized_buf a, sized_buf b);

NSData* JSONToData(id obj, NSError** outError) ;

static inline id DataToJSON(NSData* data, NSError** outError) {
    return [NSJSONSerialization JSONObjectWithData: data
                                           options: NSJSONReadingAllowFragments
                                             error: NULL];
}

id BufToJSON(sized_buf, NSError** outError);

void UpdateBuffer(void** outBuf, size_t *outLen, const void* srcBuf, size_t srcLen);
void UpdateBufferFromData(void** outBuf, size_t *outLen, NSData* data);

NSData* CompactRevID(NSString* revID);
sized_buf CompactRevIDToBuf(NSString* revID);
NSString* ExpandRevID(sized_buf compressedRevID);


typedef BOOL (^CBForest_Iterator)(const fdb_doc *doc, uint64_t bodyOffset);


@interface CBForestDB ()
@property (readonly) fdb_handle* db;
- (BOOL) _enumerateValuesFromKey: (NSData*)startKey
                           toKey: (NSData*)endKey
                         options: (const CBForestEnumerationOptions*)options
                           error: (NSError**)outError
                       withBlock: (CBForest_Iterator)block;
@end


@interface CBForestDocument ()
- (id) initWithDB: (CBForestDB*)store docID: (NSString*)docID;
- (id) initWithDB: (CBForestDB*)store info: (const fdb_doc*)info offset: (uint64_t)bodyOffset;
@property (readonly) sized_buf rawID;
@property (readonly) fdb_doc* info;
@property (readonly) uint64_t bodyFileOffset;
+ (BOOL) docInfo: (const fdb_doc*)docInfo
  matchesOptions: (const CBForestEnumerationOptions*)options;
@end
