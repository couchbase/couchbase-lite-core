//
// Query.cc
//
// Copyright 2016-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "Query.hh"
#include "DataFile.hh"
#include "Logging.hh"
#include "StringUtil.hh"


namespace litecore {

    LogDomain QueryLog("Query");


    Query::Query(DataFile &dataFile, slice expression, QueryLanguage language)
    :Logging(QueryLog)
    ,_dataFile(&dataFile)
    ,_expression(expression)
    ,_language(language)
    {
        _dataFile->registerQuery(this);
    }


    Query::~Query() {
        if (_dataFile)
            _dataFile->unregisterQuery(this);
    }


    std::string Query::loggingIdentifier() const {
        return string(_expression);
    }


    DataFile& Query::dataFile() const {
        if (!_dataFile)
            error::_throw(error::NotOpen);
        return *_dataFile;
    }


    Query::parseError::parseError(const char *message, int errPos)
    :error(error::LiteCore, error::InvalidQuery,
           format("%s near character %d", message, errPos+1))
    ,errorPosition(errPos)
    { }


}
