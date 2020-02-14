//
// Query.cc
//
// Copyright (c) 2016 Couchbase, Inc All rights reserved.
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

#include "Query.hh"
#include "KeyStore.hh"
#include "DataFile.hh"
#include "Logging.hh"
#include "StringUtil.hh"


namespace litecore {

    LogDomain QueryLog("Query");


    Query::Query(KeyStore &keyStore, slice expression, QueryLanguage language)
    :Logging(QueryLog)
    ,_keyStore(&keyStore)
    ,_expression(expression)
    ,_language(language)
    {
        keyStore.dataFile().registerQuery(this);
    }


    Query::~Query() {
        if (_keyStore)
            _keyStore->dataFile().unregisterQuery(this);
    }


    std::string Query::loggingIdentifier() const {
        return string(_expression);
    }


    KeyStore& Query::keyStore() const {
        if (!_keyStore)
            error::_throw(error::NotOpen);
        return *_keyStore;
    }


    Query::parseError::parseError(const char *message, int errPos)
    :error(error::LiteCore, error::InvalidQuery,
           format("%s near character %d", message, errPos+1))
    ,errorPosition(errPos)
    { }


}
