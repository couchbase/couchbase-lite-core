//
//  RevID.mm
//  CBForest
//
//  Created by Jens Alfke on 6/14/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#import <Foundation/Foundation.h>
#include "RevID.hh"

namespace forestdb {

     revid::operator NSString*() const {
        char expandedBuf[256];
        forestdb::slice expanded(expandedBuf, sizeof(expandedBuf));
        this->expandInto(expanded);
        return (NSString*)expanded;
    }

    revidBuffer::revidBuffer(NSString* str)
    :revid(&_buffer, 0)
    {
        slice s(str); //OPT: Could read string into local char array instead of calling -UTF8String
        parse(s);
    }

}
