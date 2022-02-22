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
#include "BLIPInternal.hh"
#include "Codec.hh"
#include "Error.hh"
#include "Logging.hh"
#include "StringUtil.hh"
#include "varint.hh"
#include <ostream>

using namespace std;
using namespace fleece;

namespace litecore { namespace blip {

#pragma mark - MESSAGE BUILDER:

    
    MessageBuilder::MessageBuilder(slice profile)
    {
        if (profile)
            setProfile(profile);
    }


    MessageBuilder::MessageBuilder(MessageIn *inReplyTo)
    :MessageBuilder()
    {
        DebugAssert(!inReplyTo->isResponse());
        type = kResponseType;
        urgent = inReplyTo->urgent();
    }


    MessageBuilder::MessageBuilder(initializer_list<property> properties)
    :MessageBuilder()
    {
        addProperties(properties);
    }


    void MessageBuilder::setProfile(slice profile) {
        addProperty(kProfileProperty, profile);
    }


    MessageBuilder& MessageBuilder::addProperties(initializer_list<property> properties) {
        for (const property &p : properties)
            addProperty(p.first, p.second);
        return *this;
    }


    void MessageBuilder::makeError(Error err) {
        DebugAssert(err.domain && err.code);
        type = kErrorType;
        addProperty(kErrorDomainProperty, err.domain);
        addProperty(kErrorCodeProperty, err.code);
        write(err.message);
    }


    FrameFlags MessageBuilder::flags() const {
        int flags = type & kTypeMask;
        if (urgent)     flags |= kUrgent;
        if (compressed) flags |= kCompressed;
        if (noreply)    flags |= kNoReply;
        return (FrameFlags)flags;
    }


    // Abbreviates certain special strings as a single byte
    void MessageBuilder::writeTokenizedString(ostream &out, slice str) {
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
        char valueStr[30];
        return addProperty(name, slice(valueStr, sprintf(valueStr, "%lld", (long long)value)));
    }


    void MessageBuilder::finishProperties() {
        if (!_wroteProperties) {
            string properties = _properties.str();
            _properties.clear();
            size_t propertiesSize = properties.size();
            if (propertiesSize > kMaxPropertiesSize)
                throw std::runtime_error("properties excessively large");
            char buf[kMaxVarintLen64];
            slice encodedSize(buf, PutUVarInt(buf, propertiesSize));
            _out.writeRaw(encodedSize);
            _out.writeRaw(slice(properties));
            _wroteProperties = true;
        }
    }


    MessageBuilder& MessageBuilder::write(slice data) {
        if(!_wroteProperties)
            finishProperties();
        _out.writeRaw(data);
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

} }
