//
//  JSONEndpoint.hh
//  LiteCore
//
//  Created by Jens Alfke on 8/19/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "Endpoint.hh"
#include "FilePath.hh"
#include <fstream>


class JSONEndpoint : public Endpoint {
public:
    JSONEndpoint(const string &spec)
    :Endpoint(spec)
    { }

    virtual void prepare(bool readOnly, bool mustExist, slice docIDProperty) override;
    virtual void copyTo(Endpoint*, uint64_t limit) override;
    virtual void writeJSON(slice docID, slice json) override;

private:
    unique_ptr<ifstream> _in;
    unique_ptr<ofstream> _out;
};
