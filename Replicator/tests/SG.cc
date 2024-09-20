//
// Created by Callum Birks on 01/11/2022.
//

#include "SG.hh"
#include "c4Database.hh"
#include <utility>
#include <map>
#include "Response.hh"
#include "StringUtil.hh"
#include "Error.hh"
#include "HTTPLogic.hh"
#include "catch.hpp"

using namespace std::string_literals;
using namespace fleece;

std::unique_ptr<REST::Response> SG::createRequest(const std::string& method, C4CollectionSpec collectionSpec,
                                                  std::string path, slice body, bool admin, double timeout,
                                                  bool logRequests) const {
    auto port = uint16_t(address.port + admin);
    if ( !hasPrefix(path, "/") ) {
        path = std::string("/") + path;
        if ( remoteDBName.size > 0 ) {
            auto kspace = (std::string)(slice)(remoteDBName);
            if ( collectionSpec != kC4DefaultCollectionSpec && collectionSpec.name.size > 0 ) {
                kspace += ".";
                if ( collectionSpec.scope.size > 0 ) { kspace += std::string(collectionSpec.scope) + "."; }
                kspace += std::string(collectionSpec.name);
            }
            path = std::string("/") + kspace + path;
        }
    }
    if ( logRequests ) {
        C4Log("*** Server command: %s %.*s:%d%s", method.c_str(), SPLAT(address.hostname), port, path.c_str());
        C4Log("Body: %.*s", SPLAT(body));
    }

    Encoder enc;
    enc.beginDict();
    enc["Content-Type"_sl] = "application/json";
    enc.endDict();
    auto headers = enc.finishDoc();

    alloc_slice authHeader_;

    if ( admin ) {
        authHeader_ = HTTPLogic::basicAuth(adminUsername, adminPassword);
    } else {
        if ( authHeader ) { authHeader_ = authHeader; }
    }

    std::string scheme = Address::isSecure(address) ? "https" : "http";
    auto r = std::make_unique<REST::Response>(scheme, method, (std::string)(slice)(address.hostname), port, path);
    r->setHeaders(headers).setBody(body).setTimeout(timeout);
    if ( pinnedCert ) r->allowOnlyCert(pinnedCert);
    if ( authHeader_ ) r->setAuthHeader(authHeader_);
    if ( proxy ) r->setProxy(*(proxy));
#ifdef COUCHBASE_ENTERPRISE
    if ( identityCert ) r->setIdentity(identityCert, identityKey);
#endif
    return r;
}

alloc_slice SG::runRequest(const std::string& method, C4CollectionSpec collectionSpec, const std::string& path,
                           slice body, bool admin, C4Error* outError, HTTPStatus* outStatus, double timeout,
                           bool logRequests) const {
    auto r = createRequest(method, collectionSpec, path, body, admin, timeout, logRequests);
    if ( r->run() ) {
        if ( outStatus ) *outStatus = r->status();
        if ( outError ) *outError = {};
        return r->body();
    } else {
        Assert(r->error().code != 0);
        if ( outStatus ) *outStatus = HTTPStatus::undefined;
        if ( outError ) *outError = r->error();

        if ( r->error() == C4Error{NetworkDomain, kC4NetErrTimeout} ) {
            C4Warn("REST request %s timed out. Current timeout is %f seconds", path.c_str(), r->getTimeout());
        }

        return nullslice;
    }
}

alloc_slice SG::addChannelToJSON(slice json, slice ckey, const std::vector<std::string>& channelIDs) {
    MutableDict dict{FLMutableDict_NewFromJSON(json, nullptr)};
    if ( !dict ) {
        C4Log("ERROR: MutableDict is null, likely your JSON is bad.");
        return nullslice;
    }
    MutableArray arr = MutableArray::newArray();
    for ( const auto& chID : channelIDs ) { arr.append(chID); }
    dict.set(ckey, arr);
    return dict.toJSON();
}

alloc_slice SG::addRevToJSON(slice json, const std::string& revID) {
    MutableDict dict{FLMutableDict_NewFromJSON(json, nullptr)};
    if ( !dict ) {
        C4Log("ERROR: MutableDict is null, likely your JSON is bad.");
        return nullslice;
    }
    dict.set("_rev", revID);
    return dict.toJSON();
}

bool SG::createUser(const std::string& username, const std::string& password) const {
    std::string body = R"({"name":")" + username + R"(","password":")" + password + "\"}";
    HTTPStatus  status;
    // Delete the user incase they already exist
    deleteUser(username);
    auto response = runRequest("POST", {}, "_user", body, true, nullptr, &status);
    return status == HTTPStatus::Created;
}

bool SG::deleteUser(const std::string& username) const {
    HTTPStatus status;
    auto       response = runRequest("DELETE", {}, "_user/"s + username, nullslice, true, nullptr, &status);
    return status == HTTPStatus::OK;
}

bool SG::assignUserChannel(const std::string& username, const std::vector<C4CollectionSpec>& collectionSpecs,
                           const std::vector<std::string>& channelIDs) const {
    // Compares the ASCII bytes of the version std::strings
    if ( getServerName() < "Couchbase Sync Gateway/3.1" ) {
        C4Log("[SG] Assigning user channels for SG version < 3.1");
        return assignUserChannelOld(username, channelIDs);
    } else {
        C4Log("[SG] Assigning user channels for SG version >= 3.1");
    }


    std::multimap<slice, slice> specsMap;
    for ( const auto& spec : collectionSpecs ) { specsMap.insert({spec.scope, spec.name}); }

    Encoder enc;
    enc.beginDict();
    enc.writeKey("collection_access"_sl);
    {
        enc.beginDict();  // collection access
        // For each unique key (scope)
        for ( auto it = specsMap.begin(), end = specsMap.end(); it != end; it = specsMap.upper_bound(it->first) ) {
            enc.writeKey(it->first);  // scope name
            enc.beginDict();          // scope
            // For each value belonging to that key (spec name belonging to that scope)
            auto collsInThisScope = specsMap.equal_range(it->first);
            for ( auto i = collsInThisScope.first; i != collsInThisScope.second; ++i ) {
                enc.writeKey(i->second);  // collection name
                enc.beginDict();          // collection
                enc.writeKey("admin_channels"_sl);
                {
                    enc.beginArray();
                    for ( const auto& chID : channelIDs ) { enc.writeString(chID); }
                    enc.endArray();
                }
                enc.endDict();  // collection
            }
            enc.endDict();  // scope
        }
        enc.endDict();  // collection access
    }
    enc.endDict();
    Doc   doc{enc.finish()};
    Value v = doc.root();

    HTTPStatus status;
    auto       response = runRequest("PUT", {}, "_user/"s + username, v.toJSON(), true, nullptr, &status);
    return status == HTTPStatus::OK;
}

bool SG::assignUserChannelOld(const std::string& username, const std::vector<std::string>& channelIDs) const {
    Encoder enc;
    enc.beginDict();
    enc.writeKey("admin_channels");
    enc.beginArray(channelIDs.size());
    for ( const auto& chID : channelIDs ) { enc.writeString(chID); }
    enc.endArray();
    enc.endDict();
    Doc   doc{enc.finish()};
    Value v = doc.root();

    HTTPStatus status;
    auto       response = runRequest("PUT", {}, "_user/"s + username, v.toJSON(), true, nullptr, &status);
    return status == HTTPStatus::OK;
}

bool SG::upsertDoc(C4CollectionSpec collectionSpec, const std::string& docID, slice body,
                   const std::vector<std::string>& channelIDs, C4Error* err) const {
    // Only add the "channels" field if channelIDs is not empty
    alloc_slice bodyWithChannel;
    if ( !channelIDs.empty() ) {
        bodyWithChannel = addChannelToJSON(body, "channels"_sl, channelIDs);
        if ( !bodyWithChannel ) {  // body had invalid JSON
            return false;
        }
    }

    HTTPStatus status;
    auto       response =
            runRequest("PUT", collectionSpec, docID, channelIDs.empty() ? body : bodyWithChannel, false, err, &status);
    return status == HTTPStatus::OK || status == HTTPStatus::Created;
}

bool SG::upsertDoc(C4CollectionSpec collectionSpec, const std::string& docID, const std::string& revID, slice body,
                   const std::vector<std::string>& channelIDs, C4Error* err) const {
    return upsertDoc(collectionSpec, docID, addRevToJSON(body, revID), channelIDs, err);
}

bool SG::upsertDocWithEmptyChannels(C4CollectionSpec collectionSpec, const std::string& docID, slice body,
                                    C4Error* err) const {
    alloc_slice bodyWithChannel = addChannelToJSON(body, "channels"_sl, {});
    if ( !bodyWithChannel ) {  // body had invalid JSON
        return false;
    }
    HTTPStatus status;
    auto       response = runRequest("PUT", collectionSpec, docID, bodyWithChannel, false, err, &status);
    return status == HTTPStatus::OK || status == HTTPStatus::Created;
}

alloc_slice SG::getServerName() const {
    auto r = createRequest("GET", {}, "/");
    if ( r->run() ) {
        Assert(r->status() == HTTPStatus::OK);
        alloc_slice serverName{r->header("Server")};
        return serverName;
    }
    return nullslice;
}

void SG::flushDatabase() const { auto response = runRequest("POST", {}, "_flush", nullslice, true); }

bool SG::insertBulkDocs(C4CollectionSpec collectionSpec, const slice docsDict, double timeout) const {
    HTTPStatus status;
    auto response = runRequest("POST", collectionSpec, "_bulk_docs", docsDict, false, nullptr, &status, timeout, false);
    return status == HTTPStatus::Created;
}

alloc_slice SG::getDoc(const std::string& docID, C4CollectionSpec collectionSpec) const {
    HTTPStatus status;
    auto       result = runRequest("GET", collectionSpec, docID, nullslice, false, nullptr, &status);
    Assert(status == HTTPStatus::OK);
    return result;
}

alloc_slice SG::sendRemoteRequest(const std::string& method, C4CollectionSpec collectionSpec, std::string path,
                                  HTTPStatus* outStatus, C4Error* outError, slice body, bool admin, bool logRequests) {
    if ( method != "GET" ) Assert(slice(remoteDBName).hasPrefix("scratch"_sl));

    auto r = createRequest(method, collectionSpec, std::move(path), body, admin, logRequests);

    if ( r->run() ) {
        *outStatus = r->status();
        *outError  = {};
        return r->body();
    } else {
        Assert(r->error().code != 0);
        *outStatus = HTTPStatus::undefined;
        *outError  = r->error();
        return nullslice;
    }
}

alloc_slice SG::sendRemoteRequest(const std::string& method, C4CollectionSpec collectionSpec, std::string path,
                                  slice body, bool admin, HTTPStatus expectedStatus, bool logRequests) {
    if ( method == "PUT" && expectedStatus == HTTPStatus::OK ) expectedStatus = HTTPStatus::Created;
    HTTPStatus  status;
    C4Error     error;
    alloc_slice response =
            sendRemoteRequest(method, collectionSpec, std::move(path), &status, &error, body, admin, logRequests);
    if ( error.code ) FAIL("Error: " << c4error_descriptionStr(error));
    INFO("Status: " << (int)status);
    Assert(status == expectedStatus);
    return response;
}
