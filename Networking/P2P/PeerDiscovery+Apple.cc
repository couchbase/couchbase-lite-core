//
// Created by Jens Alfke on 2/3/25.
//

#ifdef __APPLE__

#include "PeerDiscovery+Apple.hh"
#include "Error.hh"
#include "Logging.hh"
#include "NetworkInterfaces.hh"
#include "StringUtil.hh"
#include <dns_sd.h>
#include <netinet/in.h>

namespace litecore::p2p {
    using namespace std;

    struct BonjourProvider;


    static string sServiceType;

    static LogDomain P2PLog("P2P");    //FIXME: Should be cross platform

    static BonjourProvider* sProvider;


    #pragma mark - BONJOUR PEER:


    class BonjourPeer : public C4Peer {
    public:
        static string makeID(string const& name, string const& domain) {
            return name + "." + sServiceType + "." + domain;
        }

        BonjourPeer(string name, uint32_t interface, string domain)
        :C4Peer(makeID(name, domain), name)
        ,_domain(std::move(domain))
        ,_interface(interface)
        { }

        bool setTxtRecord(slice txt) {
            if (txt.size == 1 && txt[0] == 0)   // empty record consists of a single 00 byte
                txt = nullslice;
            if (txt == _txtRecord)
                return false;
            _txtRecord = txt;

            unordered_map<string, alloc_slice> metadata;
            if (_txtRecord) {
                auto txtLen = uint16_t(_txtRecord.size);
                unsigned count = TXTRecordGetCount(txtLen, _txtRecord.buf);
                char key[256];
                for (unsigned i = 0; i < count; i++) {
                    uint8_t valueLen;
                    const void* value;
                    auto err = TXTRecordGetItemAtIndex(txtLen, _txtRecord.buf, uint16_t(i), sizeof(key), key, &valueLen, &value);
                    if (err) break;
                    metadata.emplace(key, alloc_slice(value, valueLen));
                }
            }
            C4PeerDiscoveryProvider::setMetadata(this, std::move(metadata));
            return true;
        }

        void resolved(struct sockaddr const& addr, int ttl) {
            C4PeerAddress address;
            memcpy(&address.data, &addr, sizeof(addr));
            if (addr.sa_family == AF_INET6) {
                address.type = C4PeerAddress::IPv6;
                reinterpret_cast<sockaddr_in6*>(&address.data)->sin6_port = htons(ttl);
            } else {
                assert(addr.sa_family == AF_INET);
                address.type = C4PeerAddress::IPv4;
                reinterpret_cast<sockaddr_in*>(&address.data)->sin_port = htons(ttl);
            }
            address.expiration = c4_now() + 1000LL * ttl;
            C4PeerDiscoveryProvider::setAddresses(this, {&address, 1});
        }

        void resolveFailed(DNSServiceErrorType err) {
            auto c4error = C4Error::make(NetworkDomain, kC4NetErrHostDown);//TODO: better error
            C4PeerDiscoveryProvider::setAddresses(this, {}, c4error);
        }

        void removed() {
            if (_monitorTxtRef)
                DNSServiceRefDeallocate(_monitorTxtRef);
            if (_resolveRef)
                DNSServiceRefDeallocate(_resolveRef);
            if (_getAddrRef)
                DNSServiceRefDeallocate(_getAddrRef);
        }

        string          _domain;
        uint32_t        _interface {};
        uint16_t        _port {};
        alloc_slice     _txtRecord;
        DNSServiceRef   _monitorTxtRef {};  // reference to DNSServiceQueryRecord task
        DNSServiceRef   _resolveRef {};     // reference to DNSServiceResolve task
        DNSServiceRef   _getAddrRef {};     // reference to DNSServiceGetAddrInfo task
    };


#pragma mark - BROWSER IMPL:


    /// Internal implementation class of BonjourBrowser.
    /// This class owns a dispatch queue, and all calls other than constructor/destructor
    /// must be made on that queue.
    struct BonjourProvider : public Logging {
        BonjourProvider()
        :Logging(P2PLog)
        ,_queue(dispatch_queue_create("P2P Browser", DISPATCH_QUEUE_SERIAL))
        { }

        ~BonjourProvider() {
            if (_serviceRef) {
                Warn("Browser was not stopped before deallocating!");
                DNSServiceRefDeallocate(_serviceRef);
            }
            dispatch_release(_queue);
        }


        void check(DNSServiceErrorType err) const {
            if (err) {
                logError("got error %d", err);
                error(error::Network, 999, stringprintf("DNSServiceError %d", err))._throw(1);
            }
        }

        bool running() const {return _serviceRef != nullptr;}


        void start() {
            if (_serviceRef) return;

            try {
                logInfo("browsing '%s'...", sServiceType.c_str());

                check(DNSServiceCreateConnection(&_serviceRef));
                check(DNSServiceSetDispatchQueue(_serviceRef, _queue));

                // Start browser:
                auto browseCallback = [](DNSServiceRef,
                                         DNSServiceFlags flags,
                                         uint32_t interface,
                                         DNSServiceErrorType err,
                                         const char *serviceName,
                                         const char *regtype,
                                         const char *domain,
                                         void *ctx) -> void {
                    reinterpret_cast<BonjourProvider*>(ctx)->browseResult(flags, err, interface, serviceName, domain);
                };

                _browseRef = _serviceRef;
                check(DNSServiceBrowse(&_browseRef,
                                       kDNSServiceFlagsShareConnection | kDNSServiceFlagsIncludeP2P,
                                       kDNSServiceInterfaceIndexAny,
                                       sServiceType.c_str(),
                                       nullptr,
                                       browseCallback,
                                       this));

                if (auto port = _myPort)
                    registerService(port, _myTxtRecord);
                C4PeerDiscoveryProvider::browsing(true);
            } catch (...) {
                if (_serviceRef) {
                    stop();
                }
            }
        }


        void stop() {
            if (_serviceRef) {
                logInfo("stopping");
                DNSServiceRefDeallocate(_serviceRef);
                _serviceRef = _browseRef = _registerRef = nullptr;   // closing main ref closes shared refs too
                C4PeerDiscoveryProvider::browsing(false);
            }
        }


        void browseResult(DNSServiceFlags flags,
                          DNSServiceErrorType errorCode,
                          uint32_t interface,
                          const char *serviceName,
                          const char *domain)
        {
            if (errorCode) {
                logError("browse error %d", errorCode);
                stop();
            } else if (string_view(serviceName) == _myName) {
                _logVerbose("flags=%04x; found echo of my service '%s' in %s", flags, serviceName, domain);
            } else if (flags & kDNSServiceFlagsAdd) {
                logInfo("flags=%04x; found '%s' in %s", flags, serviceName, domain);
                auto peer = make_retained<BonjourPeer>(serviceName, interface, domain);
                C4PeerDiscoveryProvider::addPeer(peer);
            } else {
                logInfo("flags=%04x; lost '%s'", flags, serviceName);
                auto peer = C4PeerDiscovery::peerWithID(BonjourPeer::makeID(serviceName, domain));
                if (auto bonjourPeer = dynamic_cast<BonjourPeer*>(peer.get())) // should be true
                    bonjourPeer->removed();
                if (peer)
                    C4PeerDiscoveryProvider::removePeer(peer);
            }
        }


        //---- Monitoring TXT records:


        void monitorTxtRecord(Retained<C4Peer> c4p) {
            auto peer = dynamic_cast<BonjourPeer*>(c4p.get());
            if (peer->_monitorTxtRef)
                return;
            logInfo("monitoring TXT record of '%s'", peer->id.c_str());

            auto callback = [](DNSServiceRef,
                               DNSServiceFlags flags,
                               uint32_t interfaceIndex,
                               DNSServiceErrorType err,
                               const char* fullname,
                               uint16_t rrtype,
                               uint16_t rrclass,
                               uint16_t rdlen,
                               const void* rdata,
                               uint32_t ttl,
                               void* ctx) -> void {
                auto peer = reinterpret_cast<BonjourPeer*>(ctx);
                sProvider->monitorTxtResult(flags, err, slice(rdata, rdlen), ttl, peer);
            };

            peer->_monitorTxtRef = _serviceRef;
            auto err = DNSServiceQueryRecord(&peer->_monitorTxtRef,
                                             kDNSServiceFlagsShareConnection,
                                             kDNSServiceInterfaceIndexAny,
                                             peer->id.c_str(),
                                             kDNSServiceType_TXT,
                                             kDNSServiceClass_IN,
                                             callback,
                                             peer);
            Assert(!err);//TEMP
        }


        void stopMonitoringTxtRecord(Retained<C4Peer> c4p) {
            auto peer = dynamic_cast<BonjourPeer*>(c4p.get());
            if (peer->_monitorTxtRef) {
                logInfo("stopped monitoring TXT record of '%s'", peer->displayName.c_str());
                DNSServiceRefDeallocate(peer->_monitorTxtRef);
                peer->_monitorTxtRef = nullptr;
            }
        }


        void monitorTxtResult(DNSServiceFlags flags,
                               DNSServiceErrorType err,
                               slice txtRecord,
                               uint32_t ttl,
                               BonjourPeer* peer) {
            if (err == 0) {
                logInfo("flags=%04x; received TXT of %s (%zu bytes; ttl %d)",
                    flags, peer->displayName.c_str(), txtRecord.size, ttl);
                peer->setTxtRecord(txtRecord);
            } else {
                logError("error %d monitoring TXT record of %s", err, peer->displayName.c_str());
            }
            // leave the monitoring task running.
        }


        //---- Resolving peer addresses:


        void resolveAddress(Retained<C4Peer> c4p) {
            auto peer = dynamic_cast<BonjourPeer*>(c4p.get());
            if (peer->_resolveRef || peer->_getAddrRef)
                return; // already resolving

            auto callback = [](DNSServiceRef sdRef,
                               DNSServiceFlags flags,
                               uint32_t interfaceIndex,
                               DNSServiceErrorType err,
                               const char *fullname,
                               const char *hostname,
                               uint16_t portBE,                  // In network byte order
                               uint16_t txtLen,
                               const unsigned char *txtRecord,
                               void *ctx)
            {
                auto peer = reinterpret_cast<BonjourPeer*>(ctx);
                sProvider->resolveResult(flags, err, fullname, hostname,
                                         ntohs(portBE), slice(txtRecord, txtLen), peer);
            };

            peer->_resolveRef = _serviceRef;
            auto err = DNSServiceResolve(&peer->_resolveRef,
                                         kDNSServiceFlagsShareConnection,
                                         peer->_interface,
                                         peer->displayName.c_str(),
                                         sServiceType.c_str(),
                                         peer->_domain.c_str(),
                                         callback,
                                         peer);
            Assert(!err);//TEMP
        }


        void resolveResult(DNSServiceFlags flags,
                           DNSServiceErrorType err,
                           const char *fullname,
                           const char *hostname,
                           uint16_t port,
                           slice txtRecord,
                           BonjourPeer* peer)
        {
            Assert(peer->_resolveRef);
            DNSServiceRefDeallocate(peer->_resolveRef);
            peer->_resolveRef = nullptr;
            if (err) {
                auto c4error = C4Error::make(NetworkDomain, kC4NetErrHostDown);//TODO: better error
                C4PeerDiscoveryProvider::setAddresses(peer, {}, c4error);
                return;
            }

            peer->_port = port;
            logInfo("flags=%04x; resolved '%s' as hostname=%s, port=%d",
                flags, fullname, hostname, port);

            auto callback = [](DNSServiceRef,
                               DNSServiceFlags flags,
                               uint32_t interface,
                               DNSServiceErrorType err,
                               const char *hostname,
                               const struct sockaddr *address,
                               uint32_t ttl,
                               void *ctx)
            {
                auto peer = reinterpret_cast<BonjourPeer*>(ctx);
                sProvider->getAddrResult(flags, interface, err, hostname, address, ttl, peer);
            };

            peer->_getAddrRef = _serviceRef;
            peer->setTxtRecord(txtRecord);

            check(DNSServiceGetAddrInfo(&peer->_getAddrRef,
                                        kDNSServiceFlagsShareConnection,
                                        peer->_interface,
                                        kDNSServiceProtocol_IPv4, // could use 0 for both ipv4 and ipv6
                                        hostname,
                                        callback,
                                        peer));
        }


        void getAddrResult(DNSServiceFlags flags,
                           uint32_t interface,
                           DNSServiceErrorType err,
                           const char *hostname,
                           const struct sockaddr *address,
                           uint32_t ttl,
                           BonjourPeer* peer) {
            Assert(peer->_getAddrRef);
            DNSServiceRefDeallocate(peer->_getAddrRef);
            peer->_getAddrRef = nullptr;
            if (err) {
                peer->resolveFailed(err);
            } else {
                logInfo("flags=%04x; got IP address of '%s' (ttl=%d)", flags, hostname, ttl);
                peer->resolved(*address, ttl);
            }
        }


        //---- Service registration / advertising:


        void registerService(uint16_t port, slice txtRecord) {
            if (!_serviceRef) return;
            Assert(_registerRef == nullptr);
            Assert(port != 0);
            logInfo("registering my service '%s' on port %d", _myName.c_str(), port);
            auto regCallback = [](DNSServiceRef,
                                  DNSServiceFlags flags,
                                  DNSServiceErrorType errorCode,
                                  const char* name,
                                  const char* regtype,
                                  const char* domain,
                                  void* ctx) {
                reinterpret_cast<BonjourProvider*>(ctx)->regResult(flags, errorCode, name, domain);
            };

            _registerRef = _serviceRef;
            auto err = DNSServiceRegister(&_registerRef,
                                          kDNSServiceFlagsShareConnection |
                                          kDNSServiceFlagsNoAutoRename,
                                          kDNSServiceInterfaceIndexAny,
                                          _myName.c_str(),
                                          sServiceType.c_str(),
                                          nullptr,
                                          nullptr,
                                          port,
                                          uint16_t(txtRecord.size),
                                          txtRecord.buf,
                                          regCallback,
                                          this);
            Assert(!err);//TEMP
        }


        void unregisterService() {
            if (_registerRef) {
                logInfo("unregistering my service '%s'", _myName.c_str());
                DNSServiceRefDeallocate(_registerRef);
                _registerRef = nullptr;
            }
        }


        void updateTxtRecord(slice txtRecord) {
            if (_registerRef) {
                // TODO
            }
        }


        void regResult(DNSServiceFlags flags,
                       DNSServiceErrorType errorCode,
                       const char* serviceName,
                       const char* domain)
        {
            if (errorCode) {
                logError("registration error %d", errorCode);
                //TODO: Detect name conflict and retry, appending number to name
                stop();
            } else if (flags & kDNSServiceFlagsAdd) {
                logInfo("flags=%04x; Registered '%s' in %s", flags, serviceName, domain);
            } else {
                logInfo("flags=%04x; Lost registration '%s'", flags, serviceName);
            }
        }


        dispatch_queue_t            _queue {};
        string                      _myName;
        uint16_t                    _myPort {};
        alloc_slice                 _myTxtRecord;
        DNSServiceRef               _serviceRef {}, _browseRef {}, _registerRef {};
        bool                        _stopping {};
    };


/*
    BonjourBrowser::BonjourBrowser(string_view serviceType, string_view myName, Observer obs)
    :Browser(serviceType, myName, std::move(obs))
    ,sProvider(new Impl(this))
    { }

    void BonjourBrowser::start() {dispatch_async(sProvider->_queue, ^{sProvider->start();});}

    void BonjourBrowser::stop() {dispatch_async(sProvider->_queue, ^{sProvider->stop();});}

    void BonjourBrowser::startMonitoring(Peer* peer) {
        Retained<BonjourPeer> rp(dynamic_cast<BonjourPeer*>(peer));
        dispatch_async(sProvider->_queue, ^{sProvider->monitorTxtRecord(rp);});
    }

    void BonjourBrowser::stopMonitoring(Peer* peer) {
        Retained<BonjourPeer> rp(dynamic_cast<BonjourPeer*>(peer));
        dispatch_async(sProvider->_queue, ^{sProvider->stopMonitoringTxtRecord(rp);});
    }

    void BonjourBrowser::resolveAddress(Peer* peer) {
        Retained<BonjourPeer> rp(dynamic_cast<BonjourPeer*>(peer));
        dispatch_async(sProvider->_queue, ^{sProvider->resolveAddress(rp);});
    }


    void BonjourBrowser::setMyPort(uint16_t port) {
        Browser::setMyPort(port);
        dispatch_async(sProvider->_queue, ^{
            sProvider->unregisterService();
            if (port)
                sProvider->registerService(port, this->myTxtRecord());
        });
    }


    void BonjourBrowser::setMyTxtRecord(alloc_slice txt) {
        Browser::setMyTxtRecord(txt);
        dispatch_async(sProvider->_queue, ^{
            sProvider->updateTxtRecord(txt);
        });
    }
*/

    void InitializeBonjourProvider(string_view serviceType) {
        if (!C4PeerDiscoveryProvider::startBrowsing) {
            sServiceType = serviceType;
            C4PeerDiscoveryProvider::startBrowsing = []() {
                if (!sProvider)
                    sProvider = new BonjourProvider();
                dispatch_async(sProvider->_queue, ^{sProvider->start();});
            };
            C4PeerDiscoveryProvider::stopBrowsing = []() {
                if (sProvider)
                    dispatch_async(sProvider->_queue, ^{sProvider->stop();});
            };
            C4PeerDiscoveryProvider::monitorMetadata = [](C4Peer* peer, bool start) {
                Retained<C4Peer> retainedPeer(peer);
                dispatch_async(sProvider->_queue, ^{
                    if (start)
                        sProvider->monitorTxtRecord(retainedPeer);
                    else
                        sProvider->stopMonitoringTxtRecord(retainedPeer);
                });
            };
            C4PeerDiscoveryProvider::resolveAddresses = [](C4Peer* peer) {
                Retained<C4Peer> retainedPeer(peer);
                dispatch_async(sProvider->_queue, ^{sProvider->resolveAddress(retainedPeer);});
            };
        }
    }

}

#endif //___APPLE__
