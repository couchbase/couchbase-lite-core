//
// MessageBuilder.cc
//
// Copyright (c) 2017 Couchbase, Inc All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
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
            addProperty("Profile"_sl, profile);
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


    MessageBuilder& MessageBuilder::addProperties(initializer_list<property> properties) {
        for (const property &p : properties)
            addProperty(p.first, p.second);
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
