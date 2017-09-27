//
//  RemoteEndpoint.hh
//  LiteCore
//
//  Created by Jens Alfke on 9/26/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "Endpoint.hh"


class RemoteEndpoint : public Endpoint {
public:
    RemoteEndpoint(const string &spec)
    :Endpoint(spec)
    { }

    virtual bool isDatabase() const override {
        return true;
    }

    virtual void prepare(bool isSource, bool mustExist, slice docIDProperty, const Endpoint*) override;
    virtual void copyTo(Endpoint *dst, uint64_t limit) override;
    virtual void writeJSON(slice docID, slice json) override;
    virtual void finish() override;

    const C4Address& address() const    {return _address;}
    C4String databaseName() const       {return _dbName;}

private:
    C4Address _address;
    C4String _dbName;
};


