//
// MessageBuilder.cc
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "MessageBuilder.hh"
#include "Error.hh"
#include "StringUtil.hh"
#include "varint.hh"
#include "fleece/Expert.hh"
#include <ostream>

using namespace std;
using namespace fleece;

namespace litecore::blip {

#pragma mark - MESSAGE BUILDER:

    MessageBuilder::MessageBuilder(slice profile) {
        if ( profile ) addProperty("Profile"_sl, profile);
    }

    MessageBuilder::MessageBuilder(MessageIn* inReplyTo) : MessageBuilder() {
        DebugAssert(!inReplyTo->isResponse());
        type   = kResponseType;
        urgent = inReplyTo->urgent();
    }

    MessageBuilder::MessageBuilder(initializer_list<property> properties) : MessageBuilder() {
        addProperties(properties);
    }

    MessageBuilder& MessageBuilder::addProperties(initializer_list<property> properties) {
        for ( const property& p : properties ) addProperty(p.first, p.second);
        return *this;
    }

    void MessageBuilder::makeError(Error err) {
        DebugAssert(err.domain && err.code);
        type = kErrorType;
        addProperty("Error-Domain"_sl, err.domain);
        addProperty("Error-Code"_sl, err.code);
        write(err.message);
    }

    FrameFlags MessageBuilder::flags() const {
        int flags = type & kTypeMask;
        if ( urgent ) flags |= kUrgent;
        if ( compressed ) flags |= kCompressed;
        if ( noreply ) flags |= kNoReply;
        return (FrameFlags)flags;
    }

    // Abbreviates certain special strings as a single byte
    void MessageBuilder::writeTokenizedString(ostream& out, slice str) {
        Assert(str.findByte('\0') == nullptr);
        out << str << '\0';
    }

    MessageBuilder& MessageBuilder::addProperty(slice name, slice value) {
        DebugAssert(!_wroteProperties);
        writeTokenizedString(_properties, name);
        writeTokenizedString(_properties, value);
        return *this;
    }

    MessageBuilder& MessageBuilder::addProperty(slice name, int64_t value) {
        constexpr size_t bufSize = 30;
        char             valueStr[bufSize];
        return addProperty(name, slice(valueStr, snprintf(valueStr, bufSize, "%lld", (long long)value)));
    }

    void MessageBuilder::finishProperties() {
        if ( !_wroteProperties ) {
            string properties = _properties.str();
            _properties.clear();
            size_t propertiesSize = properties.size();
            if ( propertiesSize > kMaxPropertiesSize ) throw std::runtime_error("properties excessively large");
            char  buf[kMaxVarintLen64];
            slice encodedSize(buf, PutUVarInt(buf, propertiesSize));
            expert(_out).writeRaw(encodedSize);
            expert(_out).writeRaw(slice(properties));
            _wroteProperties = true;
        }
    }

    MessageBuilder& MessageBuilder::write(slice data) {
        if ( !_wroteProperties ) finishProperties();
        expert(_out).writeRaw(data);
        return *this;
    }

    alloc_slice MessageBuilder::finish() {
        finishProperties();
        return _out.finish();
    }

    void MessageBuilder::reset() {
        onProgress = nullptr;
        urgent = compressed = noreply = false;
        _out.reset();
        _properties.clear();
        _wroteProperties = false;
    }

}  // namespace litecore::blip
