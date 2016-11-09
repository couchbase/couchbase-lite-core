//
//  LC_Internal.h
//  LiteCore
//
//  Created by Jens Alfke on 10/26/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#pragma once
#import "LCDatabase.h"
#import "LCDocument.h"
#import "c4.h"
#import "Fleece.h"
#import "c4Document+Fleece.h"
#import "Fleece+CoreFoundation.h"


#define UU __unsafe_unretained


NS_ASSUME_NONNULL_BEGIN


bool convertError(const C4Error&, NSError **outError);
bool convertError(const FLError&, NSError **outError);


@interface LCDatabase ()
@property (readonly, nonatomic) C4Database* c4db;
- (void) document: (LCDocument*)doc hasUnsavedChanges: (bool)unsaved;
- (void) postDatabaseChanged;
@end

@interface LCDocument ()
- (instancetype) initWithDatabase: (LCDatabase*)db
                            docID: (NSString*)docID
                        mustExist: (BOOL)mustExist
                            error: (NSError**)outError;
- (void) _noteDocChanged;
@end


class C4Transaction {
public:
    C4Transaction(C4Database *db)
    :_db(db)
    { }

    ~C4Transaction() {
        if (_active)
            c4db_endTransaction(_db, false, nullptr);
    }

    bool begin() {
        if (!c4db_beginTransaction(_db, &_error))
            return false;
        _active = true;
        return true;
    }

    bool end(bool commit) {
        NSCAssert(_active, @"Forgot to begin");
        _active = false;
        return c4db_endTransaction(_db, commit, &_error);
    }

    bool commit()               {return end(true);}
    bool abort()                {return end(false);}

    const C4Error &error()      {return _error;}

private:
C4Database *_db;
C4Error _error;
    bool _active;
};


NS_ASSUME_NONNULL_END
