//
// Created by Callum Birks on 01/11/2022.
//

#ifndef LITECORE_SGREST_HH
#define LITECORE_SGREST_HH

#include "SGConnection.hh"
#include "HTTPLogic.hh"
#include "c4Test.hh"
#include <fleece/Fleece.hh>
#include <fleece/Expert.hh>
#include <fleece/Mutable.hh>
#include <fleece/slice.hh>
#include <utility>
#include <vector>
#include <memory>

using namespace fleece;
using namespace litecore;
using namespace litecore::net;

struct SGRest {
    // base-64 of, "Administrator:password"
    static constexpr fleece::slice kAdminAuthHeader = "Basic QWRtaW5pc3RyYXRvcjpwYXNzd29yZA=="_sl;

    static alloc_slice addChannelToJSON(slice json, slice ckey, const std::vector<std::string> &channelIDs);
    static slice getServerName(const SGConnection& sgConnection);
    // Should only be used with Walrus
    static void flushDatabase(const SGConnection& sgConnection);
    static bool createUser(const SGConnection& sgConnection, const std::string& username, const std::string& password, const std::vector<std::string> &channelIDs);
    static bool deleteUser(const SGConnection& sgConnection, const std::string& username);
    static bool assignUserChannel(const SGConnection& sgConnection, const std::string& username, const std::vector<std::string>& channelIDs);
    static bool upsertDoc(const SGConnection& sgConnection, C4CollectionSpec collectionSpec, const std::string& docID,
                          slice body, const std::vector<std::string>& channelIDs, C4Error* err = nullptr);
    static bool insertBulkDocs(const SGConnection& sgConnection, C4CollectionSpec collectionSpec, slice docsDict);
    static alloc_slice getDoc(const SGConnection& sgConnection, std::string docID, C4CollectionSpec collectionSpec = kC4DefaultCollectionSpec);

    // Planning to slowly phase out usage of sendRemoteRequest in favour of higher-level functions, as above
    static alloc_slice sendRemoteRequest(
                                SGConnection sgConnection,
                                const std::string &method,
                                std::string path,
                                HTTPStatus *outStatus NONNULL,
                                C4Error *outError NONNULL,
                                slice body =nullslice,
                                bool admin =false,
                                bool logRequests =true)
    {
        return sendRemoteRequest(std::move(sgConnection), method, { }, std::move(path), outStatus, outError, body, admin, logRequests);
    }

    static alloc_slice sendRemoteRequest(
                                const SGConnection& sgConnection,
                                const std::string &method,
                                C4CollectionSpec collectionSpec,
                                std::string path,
                                HTTPStatus *outStatus NONNULL,
                                C4Error *outError NONNULL,
                                slice body =nullslice,
                                bool admin =false,
                                bool logRequests =true);


    /// Sends an HTTP request to the remote server.
    static alloc_slice sendRemoteRequest(
                                SGConnection sgConnection,
                                const std::string &method,
                                std::string path,
                                slice body =nullslice,
                                bool admin =false,
                                HTTPStatus expectedStatus = HTTPStatus::OK,
                                bool logRequests = false)
    {
        return sendRemoteRequest(std::move(sgConnection), method, { }, std::move(path), body, admin, expectedStatus, logRequests);
    }

    static alloc_slice sendRemoteRequest(
                                const SGConnection& sgConnection,
                                const std::string &method,
                                C4CollectionSpec collectionSpec,
                                std::string path,
                                slice body =nullslice,
                                bool admin =false,
                                HTTPStatus expectedStatus = HTTPStatus::OK,
                                bool logRequests =true);

    struct TestUser {
        explicit TestUser(const SGConnection& sgConnection,
                          const std::string& username,
                          const std::vector<std::string>& channels = {},
                          const std::string& password = "password")
          : _sgConnection(sgConnection), _username(username), _password(password), _channels(channels)
        {
            REQUIRE(createUser(_sgConnection, _username, _password, _channels));
            REQUIRE(assignUserChannel(_sgConnection, _username, _channels));
            _sgConnection.authHeader = HTTPLogic::basicAuth(_username, _password);
        }

        ~TestUser() {
            deleteUser(_sgConnection, _username);
        }

        alloc_slice authHeader() { return _sgConnection.authHeader; }

        bool addChannels(const std::vector<std::string>& channels);
        bool setChannels(const std::vector<std::string>& channels);
        bool revokeAllChannels();

        SGConnection _sgConnection;
        std::string _username;
        std::string _password;
        std::vector<std::string> _channels;
    };
};


#endif //LITECORE_SGREST_HH
