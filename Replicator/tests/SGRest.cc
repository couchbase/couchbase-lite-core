//
// Created by Callum Birks on 01/11/2022.
//

#include "SGRest.hh"
#include <utility>
#include "c4Test.hh"
#include "Response.hh"
#include "StringUtil.hh"

static std::unique_ptr<REST::Response> createRequest(
        const SGConnection& sgConn,
        const std::string &method,
        C4CollectionSpec collectionSpec,
        std::string path,
        slice body = nullslice,
        bool admin = false,
        bool logRequests = true
) {
    SGConnection sgConnection { sgConn };
    auto port = uint16_t(sgConnection.address.port + !!admin);
    if (!hasPrefix(path, "/")) {
        path = std::string("/") + path;
        if (sgConnection.remoteDBName.size > 0) {
            auto kspace = (string)(slice)(sgConnection.remoteDBName);
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
              method.c_str(), SPLAT(sgConnection.address.hostname), port, path.c_str());

    Encoder enc;
    enc.beginDict();
    enc["Content-Type"_sl] = "application/json";
    enc.endDict();
    auto headers = enc.finishDoc();

    if(admin) {
        sgConnection.authHeader = HTTPLogic::basicAuth("Administrator", "password");
    }

    std::string scheme = Address::isSecure(sgConnection.address) ? "https" : "http";
    auto r = std::make_unique<REST::Response>(scheme,
                                              method,
                                              (std::string)(slice)(sgConnection.address.hostname),
                                              port,
                                              path);
    r->setHeaders(headers).setBody(body).setTimeout(5);
    if (sgConnection.pinnedCert)
        r->allowOnlyCert(sgConnection.pinnedCert);
    if (sgConnection.authHeader)
        r->setAuthHeader(sgConnection.authHeader);
    if (sgConnection.proxy)
        r->setProxy(*(sgConnection.proxy));
#ifdef COUCHBASE_ENTERPRISE
    if (sgConnection.identityCert)
        r->setIdentity(sgConnection.identityCert, sgConnection.identityKey);
#endif
    return r;
}

static alloc_slice runRequest(
            const SGConnection& sgConnection,
            const std::string &method,
            C4CollectionSpec collectionSpec,
            std::string path,
            slice body = nullslice,
            bool admin = false,
            C4Error *outError = nullptr,
            HTTPStatus *outStatus = nullptr,
            bool logRequests = true
        )
{
    auto r = createRequest(sgConnection, method, collectionSpec, std::move(path), body, admin, logRequests);
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
        return nullslice;
    }
}

alloc_slice SGRest::addChannelToJSON(slice json, slice ckey, const std::vector<std::string> &channelIDs) {
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

bool SGRest::createUser(const SGConnection& sgConnection, const std::string& username, const std::string& password, const std::vector<std::string> &channelIDs) {
    std::string body = R"({"name":")" + username + R"(","password":")" + password + "\"}";
    alloc_slice bodyWithChannel = addChannelToJSON(slice(body), "admin_channels"_sl, channelIDs);
    HTTPStatus status;
    runRequest(sgConnection, "POST", { }, "_user", bodyWithChannel, true, nullptr, &status);
    return status == HTTPStatus::Created;
}

bool SGRest::deleteUser(const SGConnection& sgConnection, const string &username) {
    HTTPStatus status;
    runRequest(sgConnection, "DELETE", { }, "_user/"s+username, nullslice, true, nullptr, &status);
    return status == HTTPStatus::OK;
}

bool SGRest::assignUserChannel(const SGConnection& sgConnection, const std::string& username, const std::vector<std::string> &channelIDs) {
    alloc_slice bodyWithChannel = addChannelToJSON("{}"_sl, "admin_channels"_sl, channelIDs);
    HTTPStatus status;
    runRequest(sgConnection, "PUT", { }, "_user/"s+username, bodyWithChannel, true, nullptr, &status);
    return status == HTTPStatus::OK;
}

bool SGRest::upsertDoc(const SGConnection& sgConnection, C4CollectionSpec collectionSpec, const std::string& docID,
                       slice body, const std::vector<std::string> &channelIDs, C4Error *err) {
    // Only add the "channels" field if channelIDs is not empty
    alloc_slice bodyWithChannel = addChannelToJSON(body, "channels"_sl, channelIDs);
    HTTPStatus status;
    runRequest(sgConnection, "PUT", collectionSpec, docID,
               channelIDs.empty() ? body : bodyWithChannel, false,
               err, &status);
    return status == HTTPStatus::OK || status == HTTPStatus::Created;
}

slice SGRest::getServerName(const SGConnection& sgConnection) {
    auto r = createRequest(sgConnection, "GET", { }, "/");
    if(r->run()) {
        REQUIRE(r->status() == HTTPStatus::OK);
        return r->header("Server");
    }
    return nullslice;
}

void SGRest::flushDatabase(const SGConnection& sgConnection) {
    runRequest(sgConnection, "POST", { }, "_flush", nullslice, true);
}

bool SGRest::insertBulkDocs(const SGConnection &sgConnection, C4CollectionSpec collectionSpec, const slice docsDict) {
    HTTPStatus status;
    runRequest(sgConnection, "POST", collectionSpec, "_bulk_docs", docsDict, false, nullptr, &status, false);
    return status == HTTPStatus::Created;
}

alloc_slice SGRest::getDoc(const SGConnection &sgConnection, std::string docID, C4CollectionSpec collectionSpec) {
    HTTPStatus status;
    auto result = runRequest(sgConnection, "GET", collectionSpec, std::move(docID), nullslice, false, nullptr, &status);
    REQUIRE(status == HTTPStatus::OK);
    return result;
}


alloc_slice SGRest::sendRemoteRequest(
                            const SGConnection& sgConnection,
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
        REQUIRE(slice(sgConnection.remoteDBName).hasPrefix("scratch"_sl));

    auto r = createRequest(sgConnection, method, collectionSpec, std::move(path), body, admin, logRequests);

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

alloc_slice SGRest::sendRemoteRequest(const SGConnection& sgConnection,
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
    alloc_slice response = sendRemoteRequest(sgConnection, method, collectionSpec, std::move(path),
                                             &status, &error, body, admin, logRequests);
    if (error.code)
        FAIL("Error: " << c4error_descriptionStr(error));
    INFO("Status: " << (int)status);
    REQUIRE(status == expectedStatus);
    return response;
}

bool SGRest::TestUser::addChannels(const std::vector<std::string>& channels) {
    for(const auto& c : channels) {
        _channels.push_back(c);
    }
    return assignUserChannel(_sgConnection, _username, _channels);
}

bool SGRest::TestUser::setChannels(const std::vector<std::string>& channels) {
    _channels = channels;
    return assignUserChannel(_sgConnection, _username, _channels);
}

bool SGRest::TestUser::revokeAllChannels() {
    _channels.clear();
    return assignUserChannel(_sgConnection, _username, _channels);
}
