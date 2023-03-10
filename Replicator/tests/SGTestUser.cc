//
// Created by Callum Birks on 10/11/2022.
//

#include "SGTestUser.hh"
#include "Error.hh"

bool SG::TestUser::addChannels(const std::vector<std::string>& channels) {
    Assert(_sg);
    for ( const auto& c : channels ) { _channels.push_back(c); }
    return _sg->assignUserChannel(_username, _collectionSpecs, _channels);
}

bool SG::TestUser::setChannels(const std::vector<std::string>& channels) {
    Assert(_sg);
    _channels = channels;
    return _sg->assignUserChannel(_username, _collectionSpecs, _channels);
}

bool SG::TestUser::revokeAllChannels() {
    Assert(_sg);
    _channels.clear();
    return _sg->assignUserChannel(_username, _collectionSpecs, _channels);
}

SG::TestUser& SG::TestUser::operator=(const SG::TestUser& other) {
    if ( this == &other ) { return *this; }
    if ( _sg ) { _sg->deleteUser(_username); }
    _sg              = other._sg;
    _username        = other._username;
    _password        = other._password;
    _authHeader      = HTTPLogic::basicAuth(_username, _password);
    _channels        = other._channels;
    _collectionSpecs = other._collectionSpecs;
    return *this;
}
