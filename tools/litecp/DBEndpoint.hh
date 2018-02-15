//
// DBEndpoint.hh
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
#include "Endpoint.hh"
#include "c4Replicator.h"
#include "Stopwatch.hh"

class JSONEndpoint;
class RemoteEndpoint;


class DbEndpoint : public Endpoint {
public:
    DbEndpoint(const string &spec)
    :Endpoint(spec)
    { }

    DbEndpoint(C4Database* db);

    virtual bool isDatabase() const override {
        return true;
    }


    virtual void prepare(bool isSource, bool mustExist, slice docIDProperty, const Endpoint*) override;
    virtual void copyTo(Endpoint *dst, uint64_t limit) override;
    virtual void writeJSON(slice docID, slice json) override;
    virtual void finish() override;

    void pushToLocal(DbEndpoint&);
    void replicateWith(RemoteEndpoint&, C4ReplicatorMode push, C4ReplicatorMode pull);

    void exportTo(JSONEndpoint*);
    void importFrom(JSONEndpoint*);

    void onStateChanged(C4ReplicatorStatus status);
    void onDocError(bool pushing,
                    C4String docID,
                    C4Error error,
                    bool transient);

private:
    void enterTransaction();
    void commit();
    void startLine();

    void exportTo(Endpoint *dst, uint64_t limit);
    C4ReplicatorParameters replicatorParameters(C4ReplicatorMode push, C4ReplicatorMode pull);
    void replicate(C4Replicator*, C4Error&);

    c4::ref<C4Database> _db;
    unsigned _transactionSize {0};
    bool _inTransaction {false};

    // Replication mode only:
    Endpoint* _otherEndpoint;
    Stopwatch _stopwatch;
    double _lastElapsed {0};
    uint64_t _lastDocCount {0};
    bool _needNewline {false};

    static constexpr unsigned kMaxTransactionSize = 1000;
};


