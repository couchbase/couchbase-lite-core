//
// Created by Jens Alfke on 2/3/25.
//

#pragma once
#include "Browser.hh"

#ifdef __APPLE__

namespace litecore::p2p {

/** DNS-SD Browser for Apple platforms. */
class BonjourBrowser : public Browser {
public:
    BonjourBrowser(string_view serviceType, string_view myName, Observer obs);

    void start() override;
    void stop() override;

    void setMyPort(uint16_t) override;
    void setMyTxtRecord(alloc_slice) override;

    void resolveAddress(Peer*) override;

private:
    friend class BonjourPeer;
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

}

#endif //__APPLE__
