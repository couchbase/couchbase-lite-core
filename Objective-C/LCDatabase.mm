//
//  LCDatabase.m
//  LiteCore
//
//  Created by Jens Alfke on 10/13/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#import "LCDatabase.h"
#import "C4.h"
#import "StringBytes.hh"


static bool convertError(C4Error &c4err, NSError **outError) {
    NSCAssert(c4err.code != 0, @"No C4Error");
    static NSString* const kDomains[] = {@"LiteCore", NSPOSIXErrorDomain, @"ForestDB", @"SQLite"};
    if (outError)
        *outError = [NSError errorWithDomain: kDomains[c4err.domain] code: c4err.code userInfo: nil];
    return false;
}


@implementation LCDatabase
{
    C4Database* _db;
}


- (instancetype) initWithPath: (NSString*)directory
                        error: (NSError**)outError
{
    self = [super init];
    if (self) {
//        C4DatabaseConfig config = {
//            .flags = (kC4DB_Create | kC4DB_AutoCompact | kC4DB_Bundled),
//            .storageEngine = kC4SQLiteStorageEngine
//        };
        C4Error error;
        stringBytes b(directory);
        _db = c4db_open({b.buf, b.size}, NULL, &error);
        if (!_db)
            return convertError(error, outError), nil;
    }
    return self;
}


- (bool) close: (NSError**)outError {
    C4Error error;
    if (!c4db_close(_db, &error))
        return convertError(error, outError), false;
    _db = nullptr;
    return true;
}


- (void) dealloc {
    c4db_free(_db);
}


@end
