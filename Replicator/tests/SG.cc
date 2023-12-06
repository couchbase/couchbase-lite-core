//
// Created by Callum Birks on 24/01/2023.
//

#include "SG.hh"
#include "Error.hh"
#include "catch.hpp"
#include "StringUtil.hh"
#include "HTTPLogic.hh"
#include <fleece/slice.hh>
#include <utility>

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

alloc_slice SG::addRevToJSON(slice json, const std::string &revID) {
    MutableDict dict {FLMutableDict_NewFromJSON(json, nullptr)};
    if(!dict) {
        C4Log("ERROR: MutableDict is null, likely your JSON is bad.");
        return nullslice;
    }
    dict.set("_rev", revID);
    return dict.toJSON();
}

slice SG::getServerName() const {
    auto r = createRequest("GET", "/");
    if(r->run()) {
        Assert(r->status() == HTTPStatus::OK);
        return r->header("Server");
    }
    return nullslice;
}

void SG::flushDatabase() const {
    runRequest("POST", "_flush", nullslice, true);
}

bool SG::createUser(const std::string &username, const std::string &password) const {
    std::string body = R"({"name":")" + username + R"(","password":")" + password + "\"}";
    HTTPStatus status;
    // Delete the user incase they already exist
    deleteUser(username);
    runRequest("POST", "_user", body, true, nullptr, &status);
    return status == HTTPStatus::Created;
}

bool SG::deleteUser(const std::string &username) const {
    HTTPStatus status;
    runRequest("DELETE", std::string("_user/")+username, nullslice, true, nullptr, &status);
    return status == HTTPStatus::OK;
}

bool SG::assignUserChannel(const std::string &username, const std::vector<std::string> &channelIDs) const {
    alloc_slice channelsJson = addChannelToJSON("{}", "admin_channels", channelIDs);
    HTTPStatus status;
    runRequest("PUT", std::string("_user/"+username), channelsJson, true, nullptr, &status);
    return status == HTTPStatus::OK;
}

bool SG::upsertDoc(const std::string &docID, slice body, const std::vector<std::string> &channelIDs, C4Error *err) const {
    // Only add the "channels" field if channelIDs is not empty
    alloc_slice bodyWithChannel;
    if(!channelIDs.empty()) {
        bodyWithChannel = addChannelToJSON(body, "channels"_sl, channelIDs);
        if(!bodyWithChannel) { // body had invalid JSON
            return false;
        }
    }

    HTTPStatus status;
    runRequest("PUT", docID,
               channelIDs.empty() ? body : bodyWithChannel, false,
               err, &status);
    return status == HTTPStatus::OK || status == HTTPStatus::Created;
}

bool SG::upsertDoc(const std::string &docID, const std::string &revID, slice body,
                   const std::vector<std::string> &channelIDs, C4Error *err) const {
    return upsertDoc(docID, addRevToJSON(body, revID), channelIDs, err);
}

bool SG::insertBulkDocs(slice docsDict, double timeout) const {
    HTTPStatus status;
    runRequest("POST", "_bulk_docs", docsDict, false, nullptr, &status, timeout, false);
    return status == HTTPStatus::Created;
}

alloc_slice SG::getDoc(const std::string &docID) const {
    HTTPStatus status;
    auto result = runRequest("GET", docID, nullslice, false, nullptr, &status);
    Assert(status == HTTPStatus::OK);
    return result;
}

alloc_slice SG::sendRemoteRequest(const std::string &method, std::string path, HTTPStatus *outStatus,
                                  C4Error *outError, slice body, bool admin, bool logRequests) {
    return runRequest(method, path, body, admin, outError, outStatus, 5.0, logRequests);
}

alloc_slice
SG::sendRemoteRequest(const std::string &method, std::string path, slice body, bool admin, HTTPStatus expectedStatus,
                      bool logRequests) {
    HTTPStatus status;
    C4Error error;
    alloc_slice response = runRequest(method, path, body, admin, &error, &status, 5.0, logRequests);
    if (error.code && error.code != (int)expectedStatus)
        FAIL(std::string("Error: ") + error.description());
    INFO(std::string("Status: ") + std::to_string((int)status));
    Assert(status == expectedStatus);
    return response;
}

std::unique_ptr<REST::Response>
SG::createRequest(const std::string &method, std::string path, slice body, bool admin, double timeout,
                  bool logRequests) const {
    auto port = uint16_t(address.port + admin);
    if (!hasPrefix(path, "/")) {
        path = std::string("/") + path;
        if (remoteDBName.size > 0) {
            path = string("/") + (string)(slice)(remoteDBName) + path;
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
    r->setHeaders(headers).setBody(body).setTimeout(timeout);
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

alloc_slice
SG::runRequest(const std::string &method, const std::string &path, slice body, bool admin, C4Error *outError,
               HTTPStatus *outStatus, double timeout, bool logRequests) const {
    auto r = createRequest(method, path, body, admin, timeout, logRequests);
    if (r->run()) {
        if(outStatus)
            *outStatus = r->status();
        if(outError)
            *outError = {};
        return r->body();
    } else {
        Assert(r->error().code != 0);
        if(outStatus)
            *outStatus = r->status();
        if(outError)
            *outError = r->error();

        if (r->error() == C4Error{NetworkDomain, kC4NetErrTimeout}) {
            C4Warn("REST request %s timed out.", path.c_str());
        }

        return nullslice;
    }
}
