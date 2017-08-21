//
//  DirEndpoint.hh
//  LiteCore
//
//  Created by Jens Alfke on 8/19/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "Endpoint.hh"
#include "FilePath.hh"
#include <fstream>


class DirectoryEndpoint : public Endpoint {
public:
    DirectoryEndpoint(const string &spec)
    :Endpoint(spec)
    ,_dir(spec, "")
    { }

    virtual void prepare(bool readOnly, bool mustExist, slice docIDProperty) override;
    virtual void copyTo(Endpoint*, uint64_t limit) override;
    virtual void writeJSON(slice docID, slice json) override;

private:
    slice readFile(const string &path, alloc_slice &buffer);

    FilePath _dir;
};

