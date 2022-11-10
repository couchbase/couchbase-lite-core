//
// Created by Callum Birks on 10/11/2022.
//

#ifndef LITECORE_SGTESTUSER_HH
#define LITECORE_SGTESTUSER_HH

#include "SG.hh"

class SG::TestUser {
public:
    explicit TestUser(SG& sg,
                      const std::string& username,
                      const std::vector<std::string>& channels = {},
                      const std::string& password = "password")
            : _sg(sg), _username(username), _password(password), _channels(channels)
    {
        REQUIRE(_sg.createUser(_username, _password, _channels));
        REQUIRE(_sg.assignUserChannel(_username, _channels));
        _authHeader = HTTPLogic::basicAuth(_username, _password);
    }

    ~TestUser() {
        _sg.deleteUser(_username);
    }

    alloc_slice authHeader() { return _authHeader; }

    bool addChannels(const std::vector<std::string>& channels);
    bool setChannels(const std::vector<std::string>& channels);
    bool revokeAllChannels();

private:
    SG& _sg;
    alloc_slice _authHeader;
    std::string _username;
    std::string _password;
    std::vector<std::string> _channels;
};

#endif //LITECORE_SGTESTUSER_HH
