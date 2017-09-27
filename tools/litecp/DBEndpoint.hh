//
//  DBEndpoint.hh
//  LiteCore
//
//  Created by Jens Alfke on 8/19/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "Endpoint.hh"
#include "c4Replicator.h"

class JSONEndpoint;
class RemoteEndpoint;


class DbEndpoint : public Endpoint {
public:
    DbEndpoint(const string &spec)
    :Endpoint(spec)
    { }

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
    bool _needNewline {false};

    static constexpr unsigned kMaxTransactionSize = 1000;
};


