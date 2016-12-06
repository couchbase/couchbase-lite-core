//
//  LCDatabase.h
//  LiteCore
//
//  Created by Jens Alfke on 10/13/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#import <Foundation/Foundation.h>
@class LCDocument;

NS_ASSUME_NONNULL_BEGIN


/********
 NOTE: THIS IS A PROVISIONAL, PLACEHOLDER API, NOT THE OFFICIAL COUCHBASE LITE 2.0 API.
 It's for prototyping, experimentation, and performance testing. It will change without notice.
 Once the 2.0 API is designed, we will begin implementing that and remove these classes.
 ********/


extern NSString* const LCErrorDomain;


extern NSString* const LCDatabaseChangedNotification;


/** Callback that resolves document conflicts during a save.
    @param myVersion  The LCDocument's current in-memory properties; nil if you deleted it
    @param theirVersion  The document's current saved revision; nil if it's been deleted
    @param baseVersion  The common ancestor, if available (else nil)
    @return  The merged properties to save, or nil to give up */
typedef NSDictionary* _Nullable (^LCConflictResolver)(NSDictionary* _Nullable myVersion,
                                                      NSDictionary* _Nullable theirVersion,
                                                      NSDictionary* _Nullable baseVersion);


typedef NS_ENUM(uint32_t, LCIndexType) {
    kLCValueIndex,         ///< Regular index of property value
    kLCFullTextIndex,      ///< Full-text index
    kLCGeoIndex,           ///< Geospatial index of GeoJSON values
};


typedef struct {
    const char * _Nullable language;    ///< Language code, for FTS index, e.g. "en" or "de".
    bool ignoreDiacritics;              ///< True to ignore accents/diacritical marks.
} LCIndexOptions;


/** LiteCore database object. (Unlike CBL 1.x there is no Manager.) */
@interface LCDatabase : NSObject <NSCopying>

+ (NSString*) defaultDirectory;

- (instancetype) initWithPath: (NSString*)directory
                        error: (NSError**)outError NS_DESIGNATED_INITIALIZER;

- (instancetype) initWithName: (NSString*)name
                        error: (NSError**)outError;

- (instancetype) init NS_UNAVAILABLE;


@property (readonly) NSString* path;


- (bool) close: (NSError**)outError;

- (bool) deleteDatabase: (NSError**)outError;

+ (bool) deleteDatabaseAtPath: (NSString*)path error: (NSError**)outError;


- (bool) inTransaction: (NSError**)outError do: (bool (^)())block;


//////// DOCUMENTS:


- (nullable LCDocument*) documentWithID: (NSString*)docID;
- (nullable LCDocument*) objectForKeyedSubscript: (NSString*)docID;

- (nullable LCDocument*) documentWithID: (NSString*)docID
                              mustExist: (bool)mustExist
                                  error: (NSError**)outError;

@property (readwrite, nullable, nonatomic) LCConflictResolver conflictResolver;


@property (readonly, nonatomic) NSSet<LCDocument*>* unsavedDocuments;

- (bool) saveAllDocuments: (NSError**)outError;


//////// INDEXES:


- (bool) createIndexOn: (NSString*)propertyPath
                  type: (LCIndexType)type
               options: (nullable const LCIndexOptions*)options
                 error: (NSError**)error;

- (bool) deleteIndexOn: (NSString*)propertyPath
                  type: (LCIndexType)type;

@end

NS_ASSUME_NONNULL_END
