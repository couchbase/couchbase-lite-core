//
//  CBForestPrivate.h
//  CBForest
//
//  Created by Jens Alfke on 9/5/12.
//  Copyright (c) 2012 Couchbase, Inc. All rights reserved.
//

#import "CBForestDB.h"
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

sized_buf CopyBuf(sized_buf buf);

void UpdateBuffer(void** outBuf, size_t *outLen, NSData* data);


@interface CBForestDB ()
@property (readonly) fdb_handle* db;
@end


@interface CBForestDocument ()
- (id) initWithStore: (CBForestDB*)store docID: (NSString*)docID;
- (id) initWithStore: (CBForestDB*)store info: (fdb_doc*)info;
@property (readonly) sized_buf rawID;
@property (readonly) fdb_doc* info;
@end
