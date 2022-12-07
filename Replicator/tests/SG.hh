//
// Created by Callum Birks on 01/11/2022.
//
/**
 * This class is used in tests to:
 *     - Hold the connection config for SGW
 *     - Perform REST requests on the SGW
 * The REST requests have been abstracted into higher-level functions to make tests less convoluted and
 * reduce re-used code.
 * If you wish to set up replication with a TestUser in CollectionSGTest, you should create the TestUser before
 * doing `collectionPreamble()`, and call collectionPreamble(collectionSpecs, testUser)
 * The TestUser class's definition can be found in SGTestUser.hh/.cc
 */

#ifndef LITECORE_SG_HH
#define LITECORE_SG_HH

#include "HTTPLogic.hh"
#include "c4Test.hh"
#include "c4Certificate.hh"
#include "c4CppUtils.hh"
#include "Response.hh"
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

class SG {
public:
    class TestUser;

    SG(C4Address address_, C4String remoteDBName_) : address(address_), remoteDBName(remoteDBName_) {}
    // Will return nullslice if your json was invalid
    static alloc_slice addChannelToJSON(slice json, slice ckey, const std::vector<std::string> &channelIDs);
    slice getServerName() const;
    // Flush should only be used with Walrus
    void flushDatabase() const;
    bool createUser(const std::string& username, const std::string& password,
                    const std::vector<std::string> &channelIDs) const;
    bool deleteUser(const std::string& username) const;
    bool assignUserChannel(const std::string& username, const std::vector<std::string>& channelIDs) const;
    bool upsertDoc(C4CollectionSpec collectionSpec, const std::string& docID,
                          slice body, const std::vector<std::string>& channelIDs = {}, C4Error* err = nullptr) const;
    bool insertBulkDocs(C4CollectionSpec collectionSpec, slice docsDict, double timeout = 30.0) const;
    // Use this in the case that you want a doc which belongs to no channels
    // It's used in some tests in ReplicatorSGTest.cc to remove an existing doc from all channels
    bool upsertDocWithEmptyChannels(C4CollectionSpec collectionSpec, const std::string& docID,
                                    slice body, C4Error* err = nullptr) const;
    alloc_slice getDoc(std::string docID, C4CollectionSpec collectionSpec = kC4DefaultCollectionSpec) const;

    void setAdminCredentials(std::string username, std::string password) { adminUsername = username;
        adminPassword = password; }

    // sendRemoteRequest functions
    // Not used within this class, to be deprecated
    alloc_slice sendRemoteRequest(
                                const std::string &method,
                                std::string path,
                                HTTPStatus *outStatus NONNULL,
                                C4Error *outError NONNULL,
                                slice body =nullslice,
                                bool admin =false,
                                bool logRequests =true)
    {
        return sendRemoteRequest(method, { }, std::move(path), outStatus, outError, body, admin, logRequests);
    }

    alloc_slice sendRemoteRequest(
                                const std::string &method,
                                C4CollectionSpec collectionSpec,
                                std::string path,
                                HTTPStatus *outStatus NONNULL,
                                C4Error *outError NONNULL,
                                slice body =nullslice,
                                bool admin =false,
                                bool logRequests =true);


    /// Sends an HTTP request to the remote server.
    alloc_slice sendRemoteRequest(
                                const std::string &method,
                                std::string path,
                                slice body =nullslice,
                                bool admin =false,
                                HTTPStatus expectedStatus = HTTPStatus::OK,
                                bool logRequests = false)
    {
        return sendRemoteRequest(method, { }, std::move(path), body, admin, expectedStatus, logRequests);
    }

    alloc_slice sendRemoteRequest(
                                const std::string &method,
                                C4CollectionSpec collectionSpec,
                                std::string path,
                                slice body =nullslice,
                                bool admin =false,
                                HTTPStatus expectedStatus = HTTPStatus::OK,
                                bool logRequests =true);
// Connection spec
    C4Address address;
    C4String remoteDBName;
    alloc_slice authHeader{nullslice};
    alloc_slice pinnedCert{nullslice};
    std::shared_ptr<ProxySpec> proxy{nullptr};
    alloc_slice networkInterface{nullslice};
    // Can modify this to fit config of your CBS/SGW
    std::string adminUsername { "Administrator" };
    std::string adminPassword { "password" };
#ifdef COUCHBASE_ENTERPRISE
    c4::ref<C4Cert> remoteCert{nullptr};
    c4::ref<C4Cert> identityCert{nullptr};
    c4::ref<C4KeyPair> identityKey{nullptr};
#endif

private:
    std::unique_ptr<REST::Response> createRequest(
            const std::string &method,
            C4CollectionSpec collectionSpec,
            std::string path,
            slice body = nullslice,
            bool admin = false,
            double timeout = 5.0,
            bool logRequests = true
    ) const;
    alloc_slice runRequest(
            const std::string &method,
            C4CollectionSpec collectionSpec,
            std::string path,
            slice body = nullslice,
            bool admin = false,
            C4Error *outError = nullptr,
            HTTPStatus *outStatus = nullptr,
            double timeout = 5.0,
            bool logRequests = true
    ) const;
};


#endif //LITECORE_SG_HH
