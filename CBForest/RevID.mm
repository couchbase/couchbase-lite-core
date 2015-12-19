//
//  RevID.mm
//  CBForest
//
//  Created by Jens Alfke on 6/14/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#import <Foundation/Foundation.h>
#include "RevID.hh"
#include "Error.hh"

namespace cbforest {

     revid::operator NSString*() const {
        char expandedBuf[256];
        cbforest::slice expanded(expandedBuf, sizeof(expandedBuf));
        this->expandInto(expanded);
        return (NSString*)expanded;
    }

    revidBuffer::revidBuffer(__unsafe_unretained NSString* str)
    :revid(&_buffer, 0)
    {
        parse(nsstring_slice(str));
    }

}
