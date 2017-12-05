//
//  MessageBuilder.cc
//  blip_cpp
//
//  Created by Jens Alfke on 4/4/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "MessageBuilder.hh"
#include "BLIPInternal.hh"
#include "Codec.hh"
#include "Error.hh"
#include "Logging.hh"
#include "StringUtil.hh"
#include "varint.hh"
#include <strstream>

using namespace std;
using namespace fleece;

namespace litecore { namespace blip {


    // Property names/values that are encoded as single bytes (first is Ctrl-A, etc.)
    // Protocol v2.0. CHANGING THIS ARRAY WILL BREAK BLIP PROTOCOL COMPATIBILITY!!
    static slice kSpecialProperties[] = {
        "Profile"_sl,
        "Error-Code"_sl,
        "Error-Domain"_sl,

        "Content-Type"_sl,
        "application/json"_sl,
        "application/octet-stream"_sl,
        "text/plain; charset=UTF-8"_sl,
        "text/xml"_sl,

        "Accept"_sl,
        "Cache-Control"_sl,
        "must-revalidate"_sl,
        "If-Match"_sl,
        "If-None-Match"_sl,
        "Location"_sl,
        nullslice
    };

    static constexpr auto kNumSpecials = sizeof(kSpecialProperties) / sizeof(slice) - 1;


#pragma mark - MESSAGE BUILDER:

    
    MessageBuilder::MessageBuilder(slice profile)
    {
        if (profile)
            addProperty("Profile"_sl, profile);
    }


    MessageBuilder::MessageBuilder(MessageIn *inReplyTo)
    :MessageBuilder()
    {
        assert(!inReplyTo->isResponse());
        type = kResponseType;
        urgent = inReplyTo->urgent();
    }


    MessageBuilder::MessageBuilder(initializer_list<property> properties)
    :MessageBuilder()
    {
        addProperties(properties);
    }


    MessageBuilder& MessageBuilder::addProperties(initializer_list<property> properties) {
        for (const property &p : properties)
            addProperty(p.first, p.second);
        return *this;
    }


    void MessageBuilder::makeError(Error err) {
        assert(err.domain && err.code);
        type = kErrorType;
        addProperty("Error-Domain"_sl, err.domain);
        addProperty("Error-Code"_sl, err.code);
        write(err.message);
    }


    FrameFlags MessageBuilder::flags() const {
        int flags = type & kTypeMask;
        if (urgent)     flags |= kUrgent;
        if (compressed) flags |= kCompressed;
        if (noreply)    flags |= kNoReply;
        return (FrameFlags)flags;
    }


    uint8_t MessageBuilder::tokenizeProperty(slice property) {
        for (uint8_t i = 0; kSpecialProperties[i]; ++i) {
            if (property == kSpecialProperties[i])
                return i + 1;
        }
        return 0;
    }


    slice MessageBuilder::untokenizeProperty(slice property) {
        if (property.size == 1 && property[0] > 0 && property[0] <= kNumSpecials)
            return kSpecialProperties[property[0] - 1];
        else
            return property;
    }

    // Abbreviates certain special strings as a single byte
    void MessageBuilder::writeTokenizedString(ostream &out, slice str) {
        assert(str.findByte('\0') == nullptr);
        assert(str.size == 0  || str[0] >= 32);
        uint8_t token = tokenizeProperty(str);
        if (token) {
            uint8_t tokenized[2] = {token, 0};
            out.write((char*)&tokenized, 2);
        } else {
            out << str << '\0';
        }
    }


    MessageBuilder& MessageBuilder::addProperty(slice name, slice value) {
        assert(!_wroteProperties);
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


    alloc_slice MessageBuilder::extractOutput() {
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
