//
// Created by Callum Birks on 10/11/2022.
//
/**
 * The TestUser class allows for easy creation of a temporary user within a test. This is especially useful when
 * isolating tests by channel, and there are functions in this class which make usage of channels easier.
 *
 * The constructor will create a user on SGW with the specified username and password and assign it to the
 * given channels. The destructor will automatically delete the user from SGW, so there is no need to manually
 * delete the user as this will be called when the object goes out of scope.
 *
 * To give a user access to all channels, pass `{"*"}` to the constructor as the channels
 *
 * Typical usage looks like:
 * ` const std::string channelID = "b"
 * ` TestUser testUser { _sg, "Bob", { channelID } }
 * ` _sg.authHeader = testUser.authHeader()
 * Where _sg is an SG object with specification for the SGW you are using
 * In any test suite inheriting from ReplicatorAPITest, there is already an _sg object set up for you.
 */

#pragma once

#include <utility>

#include "SG.hh"
#include "HTTPLogic.hh"

class SG::TestUser {
  public:
    explicit TestUser() : _sg{nullptr}, _authHeader{nullslice} {}

    explicit TestUser(SG& sg, std::string username, std::vector<std::string> channels = {"*"},
                      std::vector<C4CollectionSpec> collectionSpecs = {kC4DefaultCollectionSpec},
                      std::string                   password        = "password")
        : _sg(&sg)
        , _username(std::move(username))
        , _password(std::move(password))
        , _channels(std::move(channels))
        , _collectionSpecs{std::move(collectionSpecs)} {
        _sg->createUser(_username, _password);
        _sg->assignUserChannel(_username, _collectionSpecs, _channels);
        _authHeader = HTTPLogic::basicAuth(_username, _password);
    }

    // Same as above constructor, but accepts an array parameter
    template <size_t N>
    explicit TestUser(SG& sg, const std::string& username, const std::vector<std::string>& channels = {"*"},
                      const std::array<C4CollectionSpec, N>& collectionSpecs = {kC4DefaultCollectionSpec},
                      const std::string&                     password        = "password")
        : TestUser(sg, username, channels,
                   std::vector<C4CollectionSpec>(collectionSpecs.begin(), collectionSpecs.end()), password) {}

    TestUser(TestUser& other)
        : TestUser(*(other._sg), other._username, other._channels, other._collectionSpecs, other._password) {}

    ~TestUser() {
        if ( _sg ) _sg->deleteUser(_username);
    }

    TestUser& operator=(const TestUser& other);

    [[nodiscard]] alloc_slice authHeader() const { return _authHeader; }

    bool addChannels(const std::vector<std::string>& channels);
    bool setChannels(const std::vector<std::string>& channels);
    bool revokeAllChannels();

    std::string _username;
    std::string _password;

  private:
    SG*                           _sg;
    alloc_slice                   _authHeader;
    std::vector<std::string>      _channels;
    std::vector<C4CollectionSpec> _collectionSpecs;
};
