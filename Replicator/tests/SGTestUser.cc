//
// Created by Callum Birks on 10/11/2022.
//

#include "SGTestUser.hh"
#include "Defer.hh"

bool SG::TestUser::addChannels(const std::vector<std::string>& channels) {
    for(const auto& c : channels) {
        _channels.push_back(c);
    }
    return _sg.assignUserChannel(_username, _channels);
}

bool SG::TestUser::setChannels(const std::vector<std::string>& channels) {
    _channels = channels;
    return _sg.assignUserChannel(_username, _channels);
}

bool SG::TestUser::revokeAllChannels() {
    _channels.clear();
    return _sg.assignUserChannel(_username, _channels);
}