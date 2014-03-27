//
//  CBForestPrivate.h
//  CBForest
//
//  Created by Jens Alfke on 9/5/12.
//  Copyright (c) 2012 Couchbase, Inc. All rights reserved.
//

#import "CBForest.h"
#import "CBForestDocument.h"
#import "forestdb.h"

typedef struct {
    void* buf;
    size_t size;
} sized_buf;

BOOL Check(fdb_status err, NSError** outError);

sized_buf DataToBuf(NSData* data);
sized_buf StringToBuf(NSString* docID);
NSData* BufToData(const void* buf, size_t size);
NSString* BufToString(const void* buf, size_t size);

void UpdateBuffer(void** outBuf, size_t *outLen, NSData* data);


@interface CBForest ()
@property (readonly) fdb_handle* db;
@end


@interface CBForestDocument ()
- (id) initWithStore: (CBForest*)store docID: (NSString*)docID info: (fdb_doc*)info;
@property (readonly) sized_buf rawID;
@property (readonly) fdb_doc* info;
@end
