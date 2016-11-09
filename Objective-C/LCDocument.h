//
//  LCDocument.h
//  LiteCore
//
//  Created by Jens Alfke on 10/26/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#import "LCDatabase.h"

NS_ASSUME_NONNULL_BEGIN


/********
 NOTE: THIS IS A PROVISIONAL, PLACEHOLDER API, NOT THE OFFICIAL COUCHBASE LITE 2.0 API.
 It's for prototyping, experimentation, and performance testing. It will change without notice.
 Once the 2.0 API is designed, we will begin implementing that and remove these classes.
 ********/


@interface LCDocument : NSObject

@property (readonly, nonatomic) NSString* documentID;
@property (readonly, nonatomic) LCDatabase* database;
@property (readonly, nonatomic) uint64_t sequence;

@property (readonly) bool exists;
@property (readonly) bool isDeleted;

- (bool) reload: (NSError**)error;


//////// Properties:

@property (readwrite, nullable, nonatomic) NSDictionary* properties;

- (nullable id)objectForKeyedSubscript:(NSString*)key;
- (void) setObject: (nullable id)value forKeyedSubscript:(NSString*)key;

- (bool)      boolForKey: (NSString*)key;
- (NSInteger) integerForKey: (NSString*)key;
- (float)     floatForKey: (NSString*)key;
- (double)    doubleForKey: (NSString*)key;

- (void) setBool:    (bool)b forKey: (NSString*)key;
- (void) setInteger: (NSInteger)i forKey: (NSString*)key;
- (void) setFloat:   (float)f forKey: (NSString*)key;
- (void) setDouble:  (double)d forKey: (NSString*)key;


//////// Saving:

@property (readonly, nonatomic) bool hasUnsavedChanges;

@property (readonly, nullable, nonatomic) NSDictionary* savedProperties;

- (void) revertToSaved;

- (bool) save: (NSError**)error;

- (bool) saveWithConflictResolver: (nullable LCConflictResolver)resolver
                            error: (NSError**)error;

@property (readwrite, nullable, nonatomic) LCConflictResolver conflictResolver;

- (bool) delete: (NSError**)error;

@end


extern NSString* const LCDocumentSavedNotification;


NS_ASSUME_NONNULL_END
