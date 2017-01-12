//
//  BLIPProtocol.hh
//  LiteCore
//
//  Created by Jens Alfke on 1/4/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include <stdint.h>

namespace litecore { namespace blip {

    enum MessageType: uint8_t {
        kRequestType     = 0,  // A message initiated by a peer
        kResponseType    = 1,  // A response to a Request
        kErrorType       = 2,  // A response indicating failure
        kAckRequestType  = 4,  // Acknowledgement of data received from a Request (internal)
        kAckResponseType = 5,  // Acknowledgement of data received from a Response (internal)
    };

    enum FrameFlags: uint8_t {
        kTypeMask   = 0x07,
        kCompressed = 0x08,
        kUrgent     = 0x10,
        kNoReply    = 0x20,
        kMoreComing = 0x40,     // Used only in frames, not in messages
        kMeta       = 0x80,
    };

    typedef uint64_t MessageNo;

} }
