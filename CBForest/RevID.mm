//
//  RevID.mm
//  CBForest
//
//  Created by Jens Alfke on 6/14/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#import <Foundation/Foundation.h>
#include "RevID.hh"
#include "Error.hh"

namespace forestdb {

     revid::operator NSString*() const {
        char expandedBuf[256];
        forestdb::slice expanded(expandedBuf, sizeof(expandedBuf));
        this->expandInto(expanded);
        return (NSString*)expanded;
    }

    revidBuffer::revidBuffer(__unsafe_unretained NSString* str)
    :revid(&_buffer, 0)
    {
        parse(nsstring_slice(str));
    }

}
