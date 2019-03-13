//
//  ReplicatorOptions.hh
//  LiteCore
//
//  Created by Jens Alfke on 2/20/19.
//  Copyright © 2019 Couchbase. All rights reserved.
//

#pragma once
#include "c4Replicator.h"
#include "fleece/Fleece.hh"
#include <chrono>

namespace litecore { namespace repl {

    /** Time duration unit: seconds, stored as 64-bit floating point. */
    using duration = std::chrono::nanoseconds;


    /** Replication configuration options */
    struct Options {
        using Mode = C4ReplicatorMode;
        using Validator = bool(*)(C4String docID, C4RevisionFlags, FLDict body, void *context);

        Mode                    push                    {kC4Disabled};
        Mode                    pull                    {kC4Disabled};
        fleece::AllocedDict     properties;
        Validator               pushFilter              {nullptr};
        Validator               pullValidator           {nullptr};
        void*                   callbackContext         {nullptr};

        Options()
        { }

        Options(Mode push_, Mode pull_)
        :push(push_), pull(pull_)
        { }

        template <class SLICE>
        Options(Mode push_, Mode pull_, SLICE propertiesFleece)
        :push(push_), pull(pull_), properties(propertiesFleece)
        { }

        static Options pushing(Mode mode =kC4OneShot)  {return Options(mode, kC4Disabled);}
        static Options pulling(Mode mode =kC4OneShot)  {return Options(kC4Disabled, mode);}
        static Options passive()                       {return Options(kC4Passive,kC4Passive);}

        static constexpr unsigned kDefaultCheckpointSaveDelaySecs = 5;

        duration checkpointSaveDelay() const {
            auto secs = properties[kC4ReplicatorCheckpointInterval].asInt();
            if (secs <= 0)
                secs = kDefaultCheckpointSaveDelaySecs;
            return std::chrono::seconds(secs);
        }

        fleece::Array channels() const {return arrayProperty(kC4ReplicatorOptionChannels);}
        fleece::Array docIDs() const   {return arrayProperty(kC4ReplicatorOptionDocIDs);}
        fleece::Dict headers() const  {return dictProperty(kC4ReplicatorOptionExtraHeaders);}
        fleece::slice filter() const  {return properties[kC4ReplicatorOptionFilter].asString();}
        fleece::Dict filterParams() const
                                  {return properties[kC4ReplicatorOptionFilterParams].asDict();}
        bool skipDeleted() const  {return properties[kC4ReplicatorOptionSkipDeleted].asBool();}
        bool noIncomingConflicts() const  {return properties[kC4ReplicatorOptionNoIncomingConflicts].asBool();}
        bool noOutgoingConflicts() const  {return properties[kC4ReplicatorOptionNoIncomingConflicts].asBool();}
        int progressLevel() const  {return (int)properties[kC4ReplicatorOptionProgressLevel].asInt();}

        fleece::Array arrayProperty(const char *name) const {
            return properties[name].asArray();
        }
        fleece::Dict dictProperty(const char *name) const {
            return properties[name].asDict();
        }

        /** Sets/clears the value of a property.
            Warning: This rewrites the backing store of the properties, invalidating any
            Fleece value pointers or slices previously accessed from it. */
        template <class T>
        Options& setProperty(fleece::slice name, T value) {
            fleece::Encoder enc;
            enc.beginDict();
            if (value) {
                enc.writeKey(name);
                enc << value;
            }
            for (fleece::Dict::iterator i(properties); i; ++i) {
                fleece::slice key = i.keyString();
                if (key != name) {
                    enc.writeKey(key);
                    enc.writeValue(i.value());
                }
            }
            enc.endDict();
            properties = fleece::AllocedDict(enc.finish());
            return *this;
        }

        Options& setNoIncomingConflicts() {
            return setProperty(C4STR(kC4ReplicatorOptionNoIncomingConflicts), true);
        }

        Options& setNoDeltas() {
            return setProperty(C4STR(kC4ReplicatorOptionDisableDeltas), true);
        }

        explicit operator std::string() const;
    };

} }
