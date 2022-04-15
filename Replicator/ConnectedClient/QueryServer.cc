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
#include "c4Query.hh"

namespace litecore::repl {

    QueryServer::QueryServer(Replicator *replicator)
    :Worker(replicator, "query")
    {
        registerHandler("query", &QueryServer::handleQuery);
    }


    C4Query* QueryServer::getQuery(const string &name) {
        if (auto i = _queries.find(name); i != _queries.end())
            return i->second;
        slice queryStr = _options->namedQueries()[name].asString();
        logInfo("Compiling query '%s' from %.*s", name.c_str(), FMTSLICE(queryStr));
        if (!queryStr)
            return nullptr;
        C4QueryLanguage language = (queryStr.hasPrefix("{") ? kC4JSONQuery : kC4N1QLQuery);
        Retained<C4Query> query = _db->useLocked()->newQuery(language, queryStr);
        _queries.insert({name, query});
        return query;
    }


    void QueryServer::handleQuery(Retained<blip::MessageIn> request) {
        try {
            string name(request->property("name"));
            C4Query *query = getQuery(name);
            if (!query) {
                request->respondWithError(404, "No such query");
                return;
            }

            if (!request->JSONBody().asDict()) {
                request->respondWithError(400, "Missing query parameters");
                return;
            }

            blip::MessageBuilder reply(request);
            JSONEncoder &enc = reply.jsonBody();
            enc.beginArray();
            _db->useLocked([&](C4Database*) {
                logInfo("Running named query '%s'", name.c_str());
                Stopwatch st;
                // Run the query:
                query->setParameters(request->body());
                auto e = query->run();

                // Send the column names as the first row:
                unsigned nCols = query->columnCount();
                enc.beginArray();
                for (unsigned i = 0; i < nCols; i++) {
                    enc.writeString(query->columnTitle(i));
                }
                enc.endArray();
                //enc.writeRaw("\n");

                // Now send the real rows:
                while (e.next()) {
                    enc.beginArray();
                    for (Array::iterator i(e.columns()); i; ++i) {
                        enc.writeValue(*i);
                    }
                    enc.endArray();
                    //enc.writeRaw("\n");
                }
                logInfo("...query took %.1f ms", st.elapsedMS());
            });
            enc.endArray();
            request->respond(reply);

        } catch (...) {
            C4Error err = C4Error::fromCurrentException();
            WarnError("Exception while handling query: %s", err.description().c_str());
            request->respondWithError(c4ToBLIPError(err));
        }
    }

}
