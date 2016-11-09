//
//  LCDocument.mm
//  LiteCore
//
//  Created by Jens Alfke on 10/26/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#import "LCDocument.h"
#import "LC_Internal.h"
#import "StringBytes.hh"


NSString* const LCDocumentSavedNotification = @"LCDocumentSaved";


@implementation LCDocument
{
    C4Document* _c4doc;
    C4Database* _c4db;
    FLDict _root;
    NSMutableDictionary* _properties;
}

@synthesize documentID=_documentID, database=_database, hasUnsavedChanges=_hasUnsavedChanges;
@synthesize conflictResolver=_conflictResolver;


- (instancetype) initWithDatabase: (LCDatabase*)db
                            docID: (NSString*)docID
                        mustExist: (BOOL)mustExist
                            error: (NSError**)outError
{
    self = [super init];
    if (self) {
        _database = db;
        _documentID = docID;
        _c4db = db.c4db;
        auto doc = [self readCurrentC4Doc: outError mustExist: mustExist];
        if (!doc)
            return nil;
        [self setC4Doc: doc];
    }
    return self;
}


- (void) dealloc {
    c4doc_free(_c4doc);
}


- (NSString*) description {
    return [NSString stringWithFormat: @"%@[%@]", self.class, _documentID];
}



- (bool) reload: (NSError**)outError {
    auto doc = [self readCurrentC4Doc: outError mustExist: true];
    if (!doc)
        return false;
    [self setC4Doc: doc];
    _properties = nil;
    self.hasUnsavedChanges = false;
    return true;
}


- (C4Document*) readCurrentC4Doc: (NSError**)outError mustExist: (bool)mustExist {
    stringBytes docIDBytes(_documentID);
    C4Error err;
    auto doc = c4doc_get(_c4db, docIDBytes, mustExist, &err);
    if (!doc)
        convertError(err, outError);
    return doc;
}


- (void) setC4Doc: (nullable C4Document*)doc {
    c4doc_free(_c4doc);
    _c4doc = doc;
    _root = nullptr;
    if (_c4doc) {
        C4Slice body = _c4doc->selectedRev.body;
        if (body.size > 0) {
            _root = FLValue_AsDict(FLValue_FromTrustedData({body.buf, body.size}));
            NSAssert(_root, @"Doc body is unreadable");
        }
    }
    // This method does not clear _properties nor _hasUnsavedChanges.
}


- (bool) exists {
    return (_c4doc->flags & kExists) != 0;
}


- (bool) isDeleted {
    return (_c4doc->flags & kDeleted) != 0;
}


- (uint64_t) sequence {
    return _c4doc->sequence;
}


- (void) _noteDocChanged {
    NSLog(@"*** External change to %@", self);
}


#pragma mark - PROPERTIES:


- (NSDictionary*) savedProperties {
    if (!self.exists)
        return nil;
    else if (_properties && !_hasUnsavedChanges)
        return _properties;
    else
        return [self fleeceToID: (FLValue)_root];
}


- (nullable NSDictionary*) properties {
    if (!_properties) {
        if (!self.exists)
            return nil;
        _properties = [[self fleeceToID: (FLValue)_root] mutableCopy];
    }
    return _properties;
}


- (void) setProperties:(NSDictionary *)properties {
    _properties = properties ? [properties mutableCopy] : [NSMutableDictionary dictionary];
    self.hasUnsavedChanges = true;
}


- (id) fleeceToID: (FLValue)value {
    return FLValue_GetNSObject(value, c4db_getFLSharedKeys(_c4db), nil);
}


- (FLValue) fleeceValueForKey: (UU NSString*) key {
    return FLDict_GetSharedKey(_root, stringBytes(key), c4db_getFLSharedKeys(_c4db));
}


- (nullable id) objectForKeyedSubscript:(UU NSString*)key {
    if (_properties)
        return _properties[key];
    return [self fleeceToID: [self fleeceValueForKey: key]];
}


- (void) setObject: (UU id)value forKeyedSubscript:(UU NSString*)key {
    if (!self.properties)
        _properties = [NSMutableDictionary dictionary];
    [_properties setValue: value forKey: key];
    self.hasUnsavedChanges = true;
}


static NSNumber* numberProperty(UU NSDictionary *root, UU NSString* key) {
    id obj = root[key];
    return [obj isKindOfClass: [NSNumber class]] ? obj : nil;
}


- (bool) boolForKey: (UU NSString*)key {
    if (_properties)
        return numberProperty(_properties, key).boolValue;
    return FLValue_AsBool([self fleeceValueForKey: key]);
}

- (NSInteger) integerForKey: (UU NSString*)key {
    if (_properties)
        return numberProperty(_properties, key).integerValue;
    return FLValue_AsInt([self fleeceValueForKey: key]);
}

- (float) floatForKey: (UU NSString*)key {
    if (_properties)
        return numberProperty(_properties, key).floatValue;
    return FLValue_AsFloat([self fleeceValueForKey: key]);
}

- (double) doubleForKey: (UU NSString*)key {
    if (_properties)
        return numberProperty(_properties, key).doubleValue;
    return FLValue_AsDouble([self fleeceValueForKey: key]);
}


//OPT: There should be a way to set these without boxing to NSObjects...
- (void) setBool:    (bool)b forKey: (NSString*)key      {self[key] = @(b);}
- (void) setInteger: (NSInteger)i forKey: (NSString*)key {self[key] = @(i);}
- (void) setFloat:   (float)f forKey: (NSString*)key     {self[key] = @(f);}
- (void) setDouble:  (double)d forKey: (NSString*)key    {self[key] = @(d);}


#pragma mark - SAVING:


- (bool) hasUnsavedChanges {
    return _hasUnsavedChanges;
}

- (void) setHasUnsavedChanges: (bool)unsaved {
    if (unsaved != _hasUnsavedChanges) {
        _hasUnsavedChanges = unsaved;
        [_database document: self hasUnsavedChanges: unsaved];
    }
}


- (bool) save: (NSError**)outError {
    return [self saveWithConflictResolver: (_conflictResolver ?: _database.conflictResolver)
                                 deletion: false
                                    error: outError];
}

- (bool) saveWithConflictResolver: (LCConflictResolver)resolver
                            error: (NSError**)outError
{
    return [self saveWithConflictResolver: resolver
                                 deletion: false
                                    error: outError];
}

- (bool) delete: (NSError**)outError {
    return [self saveWithConflictResolver: (_conflictResolver ?: _database.conflictResolver)
                                 deletion: true
                                    error: outError];
}

- (bool) saveWithConflictResolver: (LCConflictResolver)resolver
                         deletion: (bool)deletion
                            error: (NSError**)outError
{
    if (!_hasUnsavedChanges)
        return true;

    C4Transaction transaction(_c4db);
    if (!transaction.begin())
        return convertError(transaction.error(),  outError);

    while (true) {
        // Encode _properties to data:
        C4DocPutRequest put = {
            .deletion = deletion,
            .docID = _c4doc->docID,
            .history = &_c4doc->revID,
            .historyCount = 1,
            .save = true,
        };
        if (_properties.count) {
            auto enc = FLEncoder_New();
            FLEncoder_SetSharedKeys(enc, c4db_getFLSharedKeys(_c4db));
            FLEncoder_WriteNSObject(enc, _properties);
            FLError flErr;
            auto body = FLEncoder_Finish(enc, &flErr);
            FLEncoder_Free(enc);
            if (!body.buf)
                return convertError(flErr, outError);
            put.body = {body.buf, body.size};
        }

        // Save to database:
        C4Error err;
        C4Document* newDoc = c4doc_put(_c4db, &put, nullptr, &err);
        c4slice_free(put.body);
        if (newDoc) {
            // Save succeeded; now commit:
            if (!transaction.commit()) {
                c4doc_free(newDoc);
                return convertError(transaction.error(), outError);
            }
            // Success!
            [self setC4Doc: newDoc];
            self.hasUnsavedChanges = false;
            [NSNotificationCenter.defaultCenter postNotificationName: LCDocumentSavedNotification
                                                              object: self];
            [_database postDatabaseChanged];
            return true;
        }

        // Save failed; examine the error:
        if (!resolver || err.domain != LiteCoreDomain || err.code != kC4ErrorConflict) {
            // non-conflict error, or else no resolver available:
            return convertError(err, outError);
        }

        // OK, we need to resolve a conflict.
        // First, load the current revision of the document:
        C4Document* curDoc = [self readCurrentC4Doc: outError mustExist: true];
        if (!curDoc)
            return false;  // failed to load current doc revision

        // Get current and base properties:
        C4Slice curBody = curDoc->selectedRev.body;
        auto curRoot = FLValue_AsDict(FLValue_FromTrustedData({curBody.buf, curBody.size}));
        NSDictionary *curProperties = [self fleeceToID: (FLValue)curRoot];
        NSDictionary *baseProperties = self.savedProperties;

        // Invoke the resolver:
        NSDictionary *resolved = resolver(_properties, curProperties, baseProperties);
        if (!resolved) {
            // Resolver gave up; return conflict error:
            return convertError(err, outError);
        }

        // Update to current revision of doc:
        [self setC4Doc: curDoc];
        if ([resolved isEqual: curProperties]) {
            // Resolved doc is same as current revision, so nothing to save:
            _properties = [curProperties copy];
            self.hasUnsavedChanges = false;
            return true;
        }

        _properties = [resolved mutableCopy];
        // Now go round and try to save again...
    }
}


- (void) revertToSaved {
    _properties = nil;
    self.hasUnsavedChanges = false;
}


@end
