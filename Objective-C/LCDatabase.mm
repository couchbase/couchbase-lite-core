//
//  LCDatabase.m
//  LiteCore
//
//  Created by Jens Alfke on 10/13/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#import "LCDatabase.h"
#import "LCDocument.h"
#import "LC_Internal.h"
#import "StringBytes.hh"
#import "c4Observer.h"


NSString* const LCErrorDomain = @"LiteCore";
NSString* const LCDatabaseChangedNotification = @"LCDatabaseChanged";


@implementation LCDatabase
{
    C4Database* _c4db;
    NSMapTable<NSString*,LCDocument*>* _documents;
    NSMutableSet<LCDocument*>* _unsavedDocuments;
    C4DatabaseObserver* _c4dbObserver;
}

@synthesize c4db=_c4db, conflictResolver=_conflictResolver;


static const C4DatabaseConfig kDBConfig = {
    .flags = (kC4DB_Create | kC4DB_AutoCompact | kC4DB_Bundled | kC4DB_SharedKeys),
    .storageEngine = kC4SQLiteStorageEngine,
    .versioning = kC4RevisionTrees,
};


- (instancetype) initWithPath: (NSString*)path
                        error: (NSError**)outError
{
    self = [super init];
    if (self) {
        stringBytes b(path.stringByStandardizingPath);
        C4Error err;
        _c4db = c4db_open({b.buf, b.size}, &kDBConfig, &err);
        if (!_c4db)
            return convertError(err, outError), nil;
        _documents = [NSMapTable strongToWeakObjectsMapTable];
        _unsavedDocuments = [NSMutableSet setWithCapacity: 100];
    }
    return self;
}


- (instancetype) initWithName: (NSString*)name
                        error: (NSError**)outError
{
    return [self initWithPath: [[self.class defaultDirectory] stringByAppendingPathComponent: name]
                        error: outError];
}


- (bool) close: (NSError**)outError {
    C4Error err;
    if (!c4db_close(_c4db, &err))
        return convertError(err, outError);
    c4dbobs_free(_c4dbObserver);
    _c4dbObserver = nullptr;
    c4db_free(_c4db);
    _c4db = nullptr;
    return true;
}


+ (NSString*) defaultDirectory {
    NSSearchPathDirectory dirID = NSApplicationSupportDirectory;
#if TARGET_OS_TV
    dirID = NSCachesDirectory; // Apple TV only allows apps to store data in the Caches directory
#endif
    NSArray* paths = NSSearchPathForDirectoriesInDomains(dirID, NSUserDomainMask, YES);
    NSString* path = paths[0];
#if !TARGET_OS_IPHONE
    NSString* bundleID = [[NSBundle mainBundle] bundleIdentifier];
    NSAssert(bundleID, @"No bundle ID");
    path = [path stringByAppendingPathComponent: bundleID];
#endif
    return [path stringByAppendingPathComponent: @"LiteCore"];
}


- (void) dealloc {
    c4db_free(_c4db);
    c4dbobs_free(_c4dbObserver);
}


- (bool) deleteDatabase: (NSError**)outError {
    C4Error err;
    if (!c4db_delete(_c4db, &err))
        return convertError(err, outError);
    _c4db = nullptr;
    return true;
}


+ (bool) deleteDatabaseAtPath: (NSString*)path error: (NSError**)outError {
    stringBytes b(path.stringByStandardizingPath);
    C4Error err;
    return c4db_deleteAtPath(b, &kDBConfig, &err) || convertError(err, outError);
}


- (bool) inTransaction: (NSError**)outError do: (bool (^)())block {
    C4Transaction transaction(_c4db);
    if (outError)
        *outError = nil;

    if (!transaction.begin())
        return convertError(transaction.error(), outError);

    if (!block())
        return false;

    if (!transaction.commit())
        return convertError(transaction.error(), outError);
    [self postDatabaseChanged];
    return true;
}


#pragma mark - DOCUMENTS:


- (LCDocument*) documentWithID: (NSString*)docID
                     mustExist: (bool)mustExist
                         error: (NSError**)outError
{
    LCDocument *doc = [_documents objectForKey: docID];
    if (!doc) {
        doc = [[LCDocument alloc] initWithDatabase: self docID: docID
                                         mustExist: mustExist
                                             error: outError];
        if (!doc)
            return nil;
        [_documents setObject: doc forKey: docID];
        [self startDBObserver];
    } else {
        if (mustExist && !doc.exists) {
            // Don't return a pre-instantiated LCDocument if it doesn't exist
            convertError(C4Error{LiteCoreDomain, kC4ErrorNotFound},  outError);
            return nil;
        }
    }
    return doc;
}

- (LCDocument*) documentWithID: (NSString*)docID {
    return [self documentWithID: docID mustExist: false error: nil];
}

- (LCDocument*) objectForKeyedSubscript: (NSString*)docID {
    return [self documentWithID: docID mustExist: false error: nil];
}

- (LCDocument*) existingDocumentWithID: (NSString*)docID error: (NSError**)outError {
    return [self documentWithID: docID mustExist: true error: outError];
}


- (void) document: (LCDocument*)doc hasUnsavedChanges: (bool)unsaved {
    if (unsaved)
        [_unsavedDocuments addObject: doc];
    else
        [_unsavedDocuments removeObject: doc];
}


- (NSSet<LCDocument*>*) unsavedDocuments {
    return _unsavedDocuments;
}


- (bool) saveAllDocuments: (NSError**)outError {
    if (_unsavedDocuments.count == 0)
        return true;
    NSSet<LCDocument*>* toSave = [_unsavedDocuments copy];
    return [self inTransaction: outError do: ^bool {
        for (LCDocument* doc in toSave) {
            if (![doc save: outError])
                return false;
        }
        return true;
    }];
}

// FIX: If a transaction fails, docs that were saved during it need to be marked unsaved again.


#pragma mark - OBSERVERS:


- (void) startDBObserver {
    if (!_c4dbObserver) {
        _c4dbObserver = c4dbobs_create(_c4db, dbObserverCallback, (__bridge void*)self);
        NSAssert(_c4dbObserver, @"Couldn't create database observer");
    }
}


static void dbObserverCallback(C4DatabaseObserver* observer, void *context) {
    @autoreleasepool {
        [(__bridge LCDatabase*)context _c4DatabaseChanged];
    }
}


- (void) _c4DatabaseChanged {
    [self performSelectorOnMainThread: @selector(postDatabaseChanged) withObject: nil waitUntilDone: NO];
}


- (void) postDatabaseChanged {
    if (!_c4dbObserver || !_c4db || c4db_isInTransaction(_c4db))
        return;
    @autoreleasepool {
        NSMutableArray* docIDs = [[NSMutableArray alloc] init];

        C4Slice docIDSlices[100];
        C4SequenceNumber lastSeq;
        uint32_t nChanges;
        while (0 < (nChanges = c4dbobs_getChanges(_c4dbObserver, docIDSlices, 100, &lastSeq))) {
            for (uint32_t i = 0; i < nChanges; ++i) {
                NSString* docID = [[NSString alloc] initWithBytes: (void*)docIDSlices[i].buf
                                                           length: docIDSlices[i].size
                                                         encoding: NSUTF8StringEncoding];
                [docIDs addObject: docID];
            }
        }

        if (docIDs.count > 0) {
            [NSNotificationCenter.defaultCenter postNotificationName: LCDatabaseChangedNotification
                                                              object: self
                                                            userInfo: @{@"docIDs": docIDs}];
        }
    }
}


@end
