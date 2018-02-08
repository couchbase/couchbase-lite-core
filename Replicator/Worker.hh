//
// Worker.hh
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

#pragma once
#include "Actor.hh"
#include "BLIPConnection.hh"
#include "Message.hh"
#include "Increment.hh"
#include "Timer.hh"
#include "c4.hh"
#include "c4Private.h"
#include "FleeceCpp.hh"
#include "Error.hh"
#include <chrono>
#include <functional>


namespace litecore { namespace repl {
    class Replicator;

    /** Time duration unit: seconds, stored as 64-bit floating point. */
    using duration = std::chrono::nanoseconds;


    /** Abstract base class of Actors used by the replicator */
    class Worker : public actor::Actor, C4InstanceCounted, protected Logging {
    public:

        /** Replication configuration options */
        struct Options {
            using Mode = C4ReplicatorMode;
            using Validator = bool(*)(C4String docID, FLDict body, void *context);

            Mode                    push                    {kC4Disabled};
            Mode                    pull                    {kC4Disabled};
            fleeceapi::AllocedDict  properties;
            Validator               pullValidator           {nullptr};
            void*                   pullValidatorContext    {nullptr};

            Options()
            { }
            
            Options(Mode push_, Mode pull_, fleece::slice propertiesFleece =fleece::nullslice)
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

            fleeceapi::Array channels() const {return arrayProperty(kC4ReplicatorOptionChannels);}
            fleeceapi::Array docIDs() const   {return arrayProperty(kC4ReplicatorOptionDocIDs);}
            fleeceapi::Dict headers() const  {return dictProperty(kC4ReplicatorOptionExtraHeaders);}
            fleece::slice filter() const  {return properties[kC4ReplicatorOptionFilter].asString();}
            fleeceapi::Dict filterParams() const
                                      {return properties[kC4ReplicatorOptionFilterParams].asDict();}
            bool skipDeleted() const  {return properties[kC4ReplicatorOptionSkipDeleted].asBool();}
            bool noIncomingConflicts() const  {return properties[kC4ReplicatorOptionNoIncomingConflicts].asBool();}
            bool noOutgoingConflicts() const  {return properties[kC4ReplicatorOptionNoIncomingConflicts].asBool();}

            fleeceapi::Array arrayProperty(const char *name) const {
                return properties[name].asArray();
            }
            fleeceapi::Dict dictProperty(const char *name) const {
                return properties[name].asDict();
            }

            /** Sets/clears the value of a property.
                Warning: This rewrites the backing store of the properties, invalidating any
                Fleece value pointers or slices previously accessed from it. */
            template <class T>
            Options& setProperty(fleece::slice name, T value) {
                fleeceapi::Encoder enc;
                enc.beginDict();
                if (value) {
                    enc.writeKey(name);
                    enc << value;
                }
                for (fleeceapi::Dict::iterator i(properties); i; ++i) {
                    slice key = i.keyString();
                    if (key != name) {
                        enc.writeKey(key);
                        enc.writeValue(i.value());
                    }
                }
                enc.endDict();
                properties = fleeceapi::AllocedDict(enc.finish());
                return *this;
            }

            Options& setNoIncomingConflicts() {
                return setProperty(C4STR(kC4ReplicatorOptionNoIncomingConflicts), true);
            }

            explicit operator std::string() const;
        };

        
        using slice = fleece::slice;
        using alloc_slice = fleece::alloc_slice;
        using ActivityLevel = C4ReplicatorActivityLevel;


        struct Status : public C4ReplicatorStatus {
            Status(ActivityLevel lvl =kC4Stopped) {
                level = lvl; error = {}; progress = progressDelta = {};
            }
            C4Progress progressDelta;
        };


        /** Called by the Replicator when the BLIP connection closes. */
        void connectionClosed() {
            enqueue(&Worker::_connectionClosed);
        }

        /** Called by child actors when their status changes. */
        void childChangedStatus(Worker *task, const Status &status) {
            enqueue(&Worker::_childChangedStatus, task, status);
        }

#if !DEBUG
    protected:
#endif
        blip::Connection* connection() const                {return _connection;}

    protected:
        Worker(blip::Connection *connection,
               Worker *parent,
               Options options,
               const char *namePrefix);

        Worker(Worker *parent, const char *namePrefix);

        ~Worker();

        /** Registers a callback to run when a BLIP request with the given profile arrives. */
        template <class ACTOR>
        void registerHandler(const char *profile,
                             void (ACTOR::*method)(Retained<blip::MessageIn>)) {
            std::function<void(Retained<blip::MessageIn>)> fn(
                                        std::bind(method, (ACTOR*)this, std::placeholders::_1) );
            _connection->setRequestHandler(profile, false, asynchronize(fn));
        }

        /** Implementation of connectionClosed(). May be overridden, but call super. */
        virtual void _connectionClosed() {
            logDebug("connectionClosed");
            _connection = nullptr;
        }

        /** Convenience to send a BLIP request. */
        void sendRequest(blip::MessageBuilder& builder,
                         blip::MessageProgressCallback onProgress = nullptr);

        void gotError(const blip::MessageIn*);
        void gotError(C4Error) ;
        virtual void onError(C4Error);         // don't call this, but you can override

        /** Report less-serious errors that affect a document but don't stop replication. */
        virtual void gotDocumentError(slice docID, C4Error, bool pushing, bool transient);

        void finishedDocument(slice docID, bool pushing);

        static blip::ErrorBuf c4ToBLIPError(C4Error);
        static C4Error blipToC4Error(const blip::Error&);

        bool isOpenClient() const               {return _connection && !_connection->isServer();}
        bool isOpenServer() const               {return _connection &&  _connection->isServer();}
        bool isContinuous() const               {return _options.push == kC4Continuous
                                                     || _options.pull == kC4Continuous;}
        const Status& status() const            {return _status;}
        virtual ActivityLevel computeActivityLevel() const;
        virtual void changedStatus();
        void addProgress(C4Progress);
        void setProgress(C4Progress);

        virtual void _childChangedStatus(Worker *task, Status) { }

        virtual void afterEvent() override;

        virtual std::string loggingIdentifier() const override {return _loggingID;}

        Options _options;
        Retained<Worker> _parent;
        uint8_t _important {1};
        std::string _loggingID;

    private:
        Retained<blip::Connection> _connection;
        int _pendingResponseCount {0};
        Status _status { };
        bool _statusChanged {false};
    };

} }
