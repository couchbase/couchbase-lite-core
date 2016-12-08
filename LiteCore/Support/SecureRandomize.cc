//
//  SecureRandomize.cc
//  LiteCore
//
//  Created by Jens Alfke on 12/7/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#include "SecureRandomize.hh"
#include "Error.hh"


namespace litecore {

    void GenerateUUID(fleece::slice s) {
        // https://en.wikipedia.org/wiki/Universally_unique_identifier#Version_4_.28random.29
        Assert(s.size == SizeOfUUID);
        SecureRandomize(s);
        auto bytes = (uint8_t*)s.buf;
        bytes[6] = (bytes[6] & ~0xF0) | 0x40;    // Set upper 4 bits to 0100
        bytes[8] = (bytes[8] & ~0xC0) | 0x80;    // Set upper 2 bits to 10
    }
    
}
