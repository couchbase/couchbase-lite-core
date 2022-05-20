//
// QueryServer.cc
//
// Copyright © 2022 Couchbase. All rights reserved.
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

#include "QueryServer.hh"
#include "Replicator.hh"
#include "DBAccess.hh"
#include "MessageBuilder.hh"
#include "StringUtil.hh"
#include "c4Query.hh"

namespace litecore::repl {

    QueryServer::QueryServer(Replicator *replicator)
    :Worker(replicator, "query")
    {
        registerHandler("query", &QueryServer::handleQuery);
    }


    static bool isJSONQuery(slice queryStr) {
        return queryStr.hasPrefix("{");
    }


    Retained<C4Query> QueryServer::compileQuery(slice queryStr) {
        C4QueryLanguage language = (isJSONQuery(queryStr) ? kC4JSONQuery : kC4N1QLQuery);
        return _db->useLocked()->newQuery(language, queryStr);
    }


    C4Query* QueryServer::getNamedQuery(const string &name) {
        if (auto i = _queries.find(name); i != _queries.end())
            return i->second;
        slice queryStr = _options->namedQueries()[name].asString();
        if (!queryStr)
            return nullptr;
        logInfo("Compiling query '%s' from %.*s", name.c_str(), FMTSLICE(queryStr));
        Retained<C4Query> query = compileQuery(queryStr);
        if (query)
            _queries.insert({name, query});
        return query;
    }


    void QueryServer::handleQuery(Retained<blip::MessageIn> request) {
        try {
            Retained<C4Query> query;
            slice name = request->property("name");
            slice src = request->property("src");
            if (!name == !src) {
                request->respondWithError(blip::Error("HTTP", 400,
                                          "Exactly one of 'name' or 'src' must be given"));
                return;
            }
            if (name) {
                // Named query:
                query = getNamedQuery(string(name));
                if (!query) {
                    request->respondWithError(blip::Error("HTTP", 404, "No such query"));
                    return;
                }
                logInfo("Running named query '%.*s'", FMTSLICE(name));
            } else {
                if (!_options->allQueries()) {
                    request->respondWithError(blip::Error("HTTP", 403,
                                                          "Arbitrary queries are not allowed"));
                    return;
                }
                logInfo("Compiling requested query: %.*s", FMTSLICE(src));
                query = compileQuery(src);
                if (!query) {
                    request->respondWithError(blip::Error("HTTP", 400, "Syntax error in query"));
                    return;
                }
            }

            if (!request->JSONBody().asDict()) {
                request->respondWithError(blip::Error("HTTP", 400, "Invalid query parameter dict"));
                return;
            }

            // Now run the query:
            blip::MessageBuilder reply(request);
            JSONEncoder &enc = reply.jsonBody();
            _db->useLocked([&](C4Database*) {
                Stopwatch st;
                // Run the query:
                query->setParameters(request->body());
                auto e = query->run();
                while (e.next()) {
                    enc.beginDict();
                    unsigned col = 0;
                    for (Array::iterator i(e.columns()); i; ++i) {
                        enc.writeKey(query->columnTitle(col++));
                        enc.writeValue(*i);
                    }
                    enc.endDict();
                    enc.nextDocument(); // Writes a newline
                }
                logInfo("...query took %.1f ms", st.elapsedMS());
            });
            request->respond(reply);

        } catch (...) {
            C4Error err = C4Error::fromCurrentException();
            WarnError("Exception while handling query: %s", err.description().c_str());
            request->respondWithError(c4ToBLIPError(err));
        }
    }

}
