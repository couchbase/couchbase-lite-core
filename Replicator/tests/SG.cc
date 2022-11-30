//
// Created by Callum Birks on 01/11/2022.
//

#include "SG.hh"
#include <utility>
#include "c4Test.hh"
#include "Response.hh"
#include "StringUtil.hh"

std::unique_ptr<REST::Response> SG::createRequest(
        const std::string &method,
        C4CollectionSpec collectionSpec,
        std::string path,
        slice body,
        bool admin,
        bool logRequests
) const {
    auto port = uint16_t(address.port + !!admin);
    if (!hasPrefix(path, "/")) {
        path = std::string("/") + path;
        if (remoteDBName.size > 0) {
            auto kspace = (string)(slice)(remoteDBName);
            if (collectionSpec.name.size > 0) {
                kspace += ".";
                if (collectionSpec.scope.size > 0) {
                    kspace += string(collectionSpec.scope) + ".";
                }
                kspace += string(collectionSpec.name);
            }
            path = string("/") + kspace + path;
        }
    }
    if (logRequests)
        C4Log("*** Server command: %s %.*s:%d%s",
              method.c_str(), SPLAT(address.hostname), port, path.c_str());

    Encoder enc;
    enc.beginDict();
    enc["Content-Type"_sl] = "application/json";
    enc.endDict();
    auto headers = enc.finishDoc();

    alloc_slice authHeader_;

    if(admin) {
        authHeader_ = HTTPLogic::basicAuth(adminUsername, adminPassword);
    } else {
        if(authHeader) {
            authHeader_ = authHeader;
        }
    }

    std::string scheme = Address::isSecure(address) ? "https" : "http";
    auto r = std::make_unique<REST::Response>(scheme,
                                              method,
                                              (std::string)(slice)(address.hostname),
                                              port,
                                              path);
    r->setHeaders(headers).setBody(body).setTimeout(5);
    if (pinnedCert)
        r->allowOnlyCert(pinnedCert);
    if (authHeader_)
        r->setAuthHeader(authHeader_);
    if (proxy)
        r->setProxy(*(proxy));
#ifdef COUCHBASE_ENTERPRISE
    if (identityCert)
        r->setIdentity(identityCert, identityKey);
#endif
    return r;
}

alloc_slice SG::runRequest(
            const std::string &method,
            C4CollectionSpec collectionSpec,
            std::string path,
            slice body,
            bool admin,
            C4Error *outError,
            HTTPStatus *outStatus,
            bool logRequests
        ) const
{
    auto r = createRequest(method, collectionSpec, path, body, admin, logRequests);
    if (r->run()) {
        if(outStatus)
            *outStatus = r->status();
        if(outError)
            *outError = {};
        return r->body();
    } else {
        REQUIRE(r->error().code != 0);
        if(outStatus)
            *outStatus = HTTPStatus::undefined;
        if(outError)
            *outError = r->error();

        if (r->error() == C4Error{NetworkDomain, kC4NetErrTimeout}) {
            C4Warn("REST request %s timed out. Current timeout is %f seconds", path.c_str(), r->getTimeout());
        }

        return nullslice;
    }
}

alloc_slice SG::addChannelToJSON(slice json, slice ckey, const std::vector<std::string> &channelIDs) {
    MutableDict dict {FLMutableDict_NewFromJSON(json, nullptr)};
    if(!dict) {
        C4Log("ERROR: MutableDict is null, likely your JSON is bad.");
        return nullslice;
    }
    MutableArray arr = MutableArray::newArray();
    for (const auto& chID : channelIDs) {
        arr.append(chID);
    }
    dict.set(ckey, arr);
    return dict.toJSON();
}

bool SG::createUser(const std::string& username, const std::string& password,
                    const std::vector<std::string> &channelIDs) const {
    std::string body = R"({"name":")" + username + R"(","password":")" + password + "\"}";
    alloc_slice bodyWithChannel = addChannelToJSON(slice(body), "admin_channels"_sl, channelIDs);
    HTTPStatus status;
    // Delete the user incase they already exist
    deleteUser(username);
    runRequest("POST", {}, "_user", bodyWithChannel, true, nullptr, &status);
    return status == HTTPStatus::Created;
}

bool SG::deleteUser(const string &username) const {
    HTTPStatus status;
    runRequest("DELETE", { }, "_user/"s+username, nullslice, true, nullptr, &status);
    return status == HTTPStatus::OK;
}

bool SG::assignUserChannel(const std::string& username, const std::vector<std::string> &channelIDs) const {
    alloc_slice bodyWithChannel = addChannelToJSON("{}"_sl, "admin_channels"_sl, channelIDs);
    HTTPStatus status;
    runRequest("PUT", { }, "_user/"s+username, bodyWithChannel, true, nullptr, &status);
    return status == HTTPStatus::OK;
}

bool SG::upsertDoc(C4CollectionSpec collectionSpec, const std::string& docID,
                   slice body, const std::vector<std::string> &channelIDs, C4Error *err) const {
    // Only add the "channels" field if channelIDs is not empty
    alloc_slice bodyWithChannel = addChannelToJSON(body, "channels"_sl, channelIDs);
    HTTPStatus status;
    runRequest("PUT", collectionSpec, docID,
               channelIDs.empty() ? body : bodyWithChannel, false,
               err, &status);
    return status == HTTPStatus::OK || status == HTTPStatus::Created;
}

slice SG::getServerName() const {
    auto r = createRequest("GET", { }, "/");
    if(r->run()) {
        REQUIRE(r->status() == HTTPStatus::OK);
        return r->header("Server");
    }
    return nullslice;
}

void SG::flushDatabase() const {
    runRequest("POST", { }, "_flush", nullslice, true);
}

bool SG::insertBulkDocs(C4CollectionSpec collectionSpec, const slice docsDict) const {
    HTTPStatus status;
    runRequest("POST", collectionSpec, "_bulk_docs", docsDict, false, nullptr, &status, false);
    return status == HTTPStatus::Created;
}

alloc_slice SG::getDoc(std::string docID, C4CollectionSpec collectionSpec) const {
    HTTPStatus status;
    auto result = runRequest("GET", collectionSpec, std::move(docID), nullslice, false, nullptr, &status);
    REQUIRE(status == HTTPStatus::OK);
    return result;
}


alloc_slice SG::sendRemoteRequest(
                            const std::string &method,
                            C4CollectionSpec collectionSpec,
                            std::string path,
                            HTTPStatus *outStatus,
                            C4Error *outError,
                            slice body,
                            bool admin,
                            bool logRequests)
{
    if (method != "GET")
        REQUIRE(slice(remoteDBName).hasPrefix("scratch"_sl));

    auto r = createRequest(method, collectionSpec, std::move(path), body, admin, logRequests);

    if (r->run()) {
        *outStatus = r->status();
        *outError = {};
        return r->body();
    } else {
        REQUIRE(r->error().code != 0);
        *outStatus = HTTPStatus::undefined;
        *outError = r->error();
        return nullslice;
    }
}

alloc_slice SG::sendRemoteRequest(
                                  const std::string &method,
                                  C4CollectionSpec collectionSpec,
                                  std::string path,
                                  slice body,
                                  bool admin,
                                  HTTPStatus expectedStatus,
                                  bool logRequests)
{
    if (method == "PUT" && expectedStatus == HTTPStatus::OK)
        expectedStatus = HTTPStatus::Created;
    HTTPStatus status;
    C4Error error;
    alloc_slice response = sendRemoteRequest(method, collectionSpec, std::move(path),
                                             &status, &error, body, admin, logRequests);
    if (error.code)
        FAIL("Error: " << c4error_descriptionStr(error));
    INFO("Status: " << (int)status);
    REQUIRE(status == expectedStatus);
    return response;
}
