//
// BLIPProtocol.hh
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "fleece/slice.hh"
#include <stdint.h>

namespace litecore { namespace blip {

    // See "docs/BLIP Protocol.md"

    /// The types of messages in BLIP.
    enum MessageType: uint8_t {
        kRequestType     = 0,  ///< A message initiated by a peer
        kResponseType    = 1,  ///< A response to a Request
        kErrorType       = 2,  ///< A response indicating failure
        kAckRequestType  = 4,  // Acknowledgement of data received from a Request (internal)
        kAckResponseType = 5,  // Acknowledgement of data received from a Response (internal)
    };

    /// Array mapping MessageType to a short mnemonic like "REQ", for logging purposes.
    extern const char* const kMessageTypeNames[8];

    /// The flags at the start of a message frame, including the 3 bits containing the type.
    enum FrameFlags: uint8_t {
        kTypeMask   = 0x07,     ///< These 3 bits hold a MessageType
        kCompressed = 0x08,     ///< Message payload is gzip-deflated
        kUrgent     = 0x10,     ///< Message is given priority delivery
        kNoReply    = 0x20,     ///< Request only: no response desired
        kMoreComing = 0x40,     // Used only in frames, not in messages
    };


    /// A message number. Each peer numbers messages it sends sequentially starting at 1.
    /// Each peer's message numbers are independent.
    typedef uint64_t MessageNo;

    /// The size of a message.
    typedef uint64_t MessageSize;

    /// Implementation-imposed max encoded size of message properties (not part of protocol)
    constexpr uint64_t kMaxPropertiesSize = 100 * 1024;


    static constexpr const char* kProfilePropertyStr = "Profile";
    /// The "Profile" property contains the message's type
    static constexpr fleece::slice kProfileProperty(kProfilePropertyStr);

    /// Property in an error response giving a namespace for the error code.
    /// If omitted the default value is `kBLIPErrorDomain`.
    static constexpr fleece::slice kErrorDomainProperty = "Error-Domain";

    /// Property in an error response giving a numeric error code.
    static constexpr fleece::slice kErrorCodeProperty   = "Error-Code";

    /// The default error domain, for errors that are not app-specific.
    /// By convention its error codes are based on HTTP's, i.e. 404 for "not found".
    static constexpr fleece::slice kBLIPErrorDomain = "BLIP";

} }
