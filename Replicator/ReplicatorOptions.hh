//
//  ReplicatorOptions.hh
//
//  Copyright 2019-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "c4ReplicatorTypes.h"
#include "fleece/Fleece.hh"

namespace litecore { namespace repl {

    /** Replication configuration options */
    struct Options {

        //---- Public fields:

        using Mode = C4ReplicatorMode;
        using Validator = C4ReplicatorValidationFunction;
        using PropertyEncryptor = C4ReplicatorPropertyEncryptionCallback;
        using PropertyDecryptor = C4ReplicatorPropertyDecryptionCallback;

        Mode                    push                    {kC4Disabled};
        Mode                    pull                    {kC4Disabled};
        fleece::AllocedDict     properties;
        Validator               pushFilter              {nullptr};
        Validator               pullValidator           {nullptr};
        PropertyEncryptor       propertyEncryptor       {nullptr};
        PropertyDecryptor       propertyDecryptor       {nullptr};
        void*                   callbackContext         {nullptr};

        //---- Constructors/factories:

        Options() =default;

        Options(Mode push_, Mode pull_)
        :push(push_), pull(pull_)
        { }

        template <class SLICE>
        Options(Mode push_, Mode pull_, SLICE propertiesFleece)
        :push(push_), pull(pull_), properties(propertiesFleece)
        { }

        explicit Options(C4ReplicatorParameters params)
        :push(params.push)
        ,pull(params.pull)
        ,properties(params.optionsDictFleece)
        ,pushFilter(params.pushFilter)
        ,pullValidator(params.validationFunc)
        ,propertyEncryptor(params.propertyEncryptor)
        ,propertyDecryptor(params.propertyDecryptor)
        ,callbackContext(params.callbackContext)
        { }

        static Options pushing(Mode mode =kC4OneShot)  {return Options(mode, kC4Disabled);}
        static Options pulling(Mode mode =kC4OneShot)  {return Options(kC4Disabled, mode);}
        static Options pushpull(Mode mode =kC4OneShot) {return Options(mode, mode);}
        static Options passive()                       {return Options(kC4Passive,kC4Passive);}

        //---- Property accessors:

        fleece::Array channels() const {return arrayProperty(kC4ReplicatorOptionChannels);}
        fleece::Array docIDs() const   {return arrayProperty(kC4ReplicatorOptionDocIDs);}
        fleece::slice filter() const  {return properties[kC4ReplicatorOptionFilter].asString();}
        fleece::Dict filterParams() const
                                  {return properties[kC4ReplicatorOptionFilterParams].asDict();}
        bool skipDeleted() const  {return boolProperty(kC4ReplicatorOptionSkipDeleted);}
        bool noIncomingConflicts() const  {return boolProperty(kC4ReplicatorOptionNoIncomingConflicts);}
        bool noOutgoingConflicts() const  {return boolProperty(kC4ReplicatorOptionNoIncomingConflicts);}
        int progressLevel() const  {
            if(properties[kC4ReplicatorOptionProgressLevel]) {
                C4Warn("Passing in progress level via configuration is deprecated; use the setProgressLevel API");
            }

            return (int)properties[kC4ReplicatorOptionProgressLevel].asInt();
        }

        bool disableDeltaSupport() const {return boolProperty(kC4ReplicatorOptionDisableDeltas);}
        bool disablePropertyDecryption() const {return boolProperty(kC4ReplicatorOptionDisablePropertyDecryption);}

        bool enableAutoPurge() const {
            if (!properties[kC4ReplicatorOptionAutoPurge])
                return true;
            return boolProperty(kC4ReplicatorOptionAutoPurge);
        }

        /** Returns a string that uniquely identifies the remote database; by default its URL,
            or the 'remoteUniqueID' option if that's present (for P2P dbs without stable URLs.) */
        fleece::slice remoteDBIDString(fleece::slice remoteURL) const {
            auto uniqueID = properties[kC4ReplicatorOptionRemoteDBUniqueID].asString();
            return uniqueID ? uniqueID : remoteURL;
        }

        fleece::Array arrayProperty(const char *name) const {
            return properties[name].asArray();
        }
        fleece::Dict dictProperty(const char *name) const {
            return properties[name].asDict();
        }

        //---- Property setters (used only by tests)

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
            return setProperty(kC4ReplicatorOptionNoIncomingConflicts, true);
        }

        Options& setNoDeltas() {
            return setProperty(kC4ReplicatorOptionDisableDeltas, true);
        }

        Options& setNoPropertyDecryption() {
            return setProperty(kC4ReplicatorOptionDisablePropertyDecryption, true);
        }

        bool boolProperty(slice property) const   {return properties[property].asBool();}

        explicit operator std::string() const;
    };

} }
