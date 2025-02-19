//
// Created by Jens Alfke on 2/3/25.
//

#include <arpa/inet.h>
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


    static C4Error convertErrorCode(DNSServiceErrorType err) {
        if (err)
            return C4Error::make(NetworkDomain, 999, stringprintf("DNSServiceError %d", err));
        else
            return kC4NoError;
    }

    static void freeServiceRef(DNSServiceRef& ref) {
        if (ref) {
            DNSServiceRefDeallocate(ref);
            ref = nullptr;
        }
    }


    #pragma mark - BONJOUR PEER:


    /** C4Peer subclass created by BonjourProvider. */
    class BonjourPeer : public C4Peer {
    public:
        static string makeID(string const& name, string const& domain) {
            return name + "." + sServiceType + "." + domain;
        }

        BonjourPeer(string const& name, uint32_t interface, string domain)
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

        void resolved(struct sockaddr const& addr, uint32_t ttl) {
            char buf[INET6_ADDRSTRLEN];
            inet_ntop(addr.sa_family, &addr, buf, sizeof(buf));
            string str;
            if (addr.sa_family == AF_INET)
                str = stringprintf("%s:%d", buf, _port);
            else
                str = stringprintf("[%s]:%d", buf, _port);
            C4PeerAddress address {
                .address = std::move(str),
                .type = (addr.sa_family == AF_INET ? C4PeerAddress::IPv4 : C4PeerAddress::IPv6),
                .expiration = c4_now() + 1000LL * ttl
            };
            C4PeerDiscoveryProvider::setAddresses(this, {&address, 1});
        }

        void resolveFailed(DNSServiceErrorType err) {
            C4PeerDiscoveryProvider::setAddresses(this, {}, convertErrorCode(err));
        }

        void removed() {
            freeServiceRef(_monitorTxtRef);
            freeServiceRef(_resolveRef);
            freeServiceRef(_getAddrRef);
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


    /// Implements DNS-SD peer discovery.
    /// This class owns a dispatch queue, and all calls other than constructor/destructor
    /// must be made on that queue.
    struct BonjourProvider : public Logging {
        BonjourProvider()
        :Logging(P2PLog)
        ,queue(dispatch_queue_create("P2P Browser", DISPATCH_QUEUE_SERIAL))
        { }

        ~BonjourProvider() override {
            if (_serviceRef) {
                Warn("Browser was not stopped before deallocating!");
                freeServiceRef(_serviceRef);
            }
            dispatch_release(queue);
        }


        bool running() const {return _serviceRef != nullptr;}


        /// API request to start browsing.
        void start() {
            if (_serviceRef) return;

            DNSServiceErrorType err = 0;
            do {
                logInfo("browsing '%s'...", sServiceType.c_str());

                err = DNSServiceCreateConnection(&_serviceRef);
                if (err) break;
                err = DNSServiceSetDispatchQueue(_serviceRef, queue);
                if (err) break;

                // Start browser:
                auto browseCallback = [](DNSServiceRef,
                                         DNSServiceFlags flags,
                                         uint32_t interface,
                                         DNSServiceErrorType err,
                                         const char *serviceName,
                                         const char *regtype,
                                         const char *domain,
                                         void *ctx) -> void {
                    static_cast<BonjourProvider*>(ctx)->browseResult(flags, err, interface, serviceName, domain);
                };

                _browseRef = _serviceRef;
                err = DNSServiceBrowse(&_browseRef,
                                       kDNSServiceFlagsShareConnection | kDNSServiceFlagsIncludeP2P,
                                       kDNSServiceInterfaceIndexAny,
                                       sServiceType.c_str(),
                                       nullptr,
                                       browseCallback,
                                       this);
                if (err) break;

                C4PeerDiscoveryProvider::browsing(true);

                if (auto port = _myPort)
                    registerService(port, _myTxtRecord);
            } while (false);
            if (err)
                stop(err);
        }


        /// API request to stop browsing.
        void stop(DNSServiceErrorType err = 0) {
            bool opened = (_serviceRef != nullptr);
            if (opened) {
                logInfo("stopping");
                freeServiceRef(_serviceRef);
                // closing main ref closes shared refs too, so do not free them:
                _browseRef = _registerRef = nullptr;
            }
            if (opened || err)
                C4PeerDiscoveryProvider::browsing(false, convertErrorCode(err));
        }


        void browseResult(DNSServiceFlags flags,
                          DNSServiceErrorType err,
                          uint32_t interface,
                          const char *serviceName,
                          const char *domain)
        {
            if (err) {
                logError("browse error %d", err);
                stop(err);
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


        /// API request to monitor a peer's metadata.
        void monitorTxtRecord(Retained<C4Peer> c4peer) {
            auto peer = dynamic_cast<BonjourPeer*>(c4peer.get());
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
                auto peer = static_cast<BonjourPeer*>(ctx);
                sProvider->monitorTxtResult(flags, err, slice(rdata, rdlen), ttl, peer);
            };

            auto monitorTxtRef = _serviceRef;
            auto err = DNSServiceQueryRecord(&monitorTxtRef,
                                             kDNSServiceFlagsShareConnection,
                                             kDNSServiceInterfaceIndexAny,
                                             peer->id.c_str(),
                                             kDNSServiceType_TXT,
                                             kDNSServiceClass_IN,
                                             callback,
                                             peer);
            if (err == 0) {
                peer->_monitorTxtRef = monitorTxtRef;
            } else {
                warn("failed to monitor TXT record: err %d", err);
            }
        }


        /// API request to stop monitoring a peer's metadata.
        void stopMonitoringTxtRecord(Retained<C4Peer> c4peer) {
            auto peer = dynamic_cast<BonjourPeer*>(c4peer.get());
            if (peer->_monitorTxtRef) {
                logInfo("stopped monitoring TXT record of '%s'", peer->displayName.c_str());
                freeServiceRef(peer->_monitorTxtRef);
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

        /// API request to resolve a peer's address.
        void resolveAddress(Retained<C4Peer> c4peer) {
            // This has to be done in two asynchronous steps:
            // 1. call DNSServiceResolve to get the hostname and port.
            // 2. call DNSServiceGetAddrInfo with the hostname to get the address.

            auto peer = dynamic_cast<BonjourPeer*>(c4peer.get());
            if (peer->_resolveRef || peer->_getAddrRef)
                return; // already resolving

            auto callback = [](DNSServiceRef,
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
                auto peer = static_cast<BonjourPeer*>(ctx);
                sProvider->resolveResult(flags, err, fullname, hostname,
                                         ntohs(portBE), slice(txtRecord, txtLen), peer);
            };

            auto resolveRef = _serviceRef;
            auto err = DNSServiceResolve(&resolveRef,
                                         kDNSServiceFlagsShareConnection,
                                         peer->_interface,
                                         peer->displayName.c_str(),
                                         sServiceType.c_str(),
                                         peer->_domain.c_str(),
                                         callback,
                                         peer);
            if (err == 0) {
                peer->_resolveRef = resolveRef;
            } else {
                peer->resolveFailed(err);
            }
        }


        // completion routine of DNSServiceResolve
        void resolveResult(DNSServiceFlags flags,
                           DNSServiceErrorType err,
                           const char *fullname,
                           const char *hostname,
                           uint16_t port,
                           slice txtRecord,
                           BonjourPeer* peer)
        {
            freeServiceRef(peer->_resolveRef);
            if (err) {
                peer->resolveFailed(err);
                return;
            }

            peer->_port = port;
            logInfo("flags=%04x; resolved '%s' as hostname=%s, port=%d",
                flags, fullname, hostname, port);

            peer->setTxtRecord(txtRecord);

            auto callback = [](DNSServiceRef,
                               DNSServiceFlags flags,
                               uint32_t interface,
                               DNSServiceErrorType err,
                               const char *hostname,
                               const struct sockaddr *address,
                               uint32_t ttl,
                               void *ctx)
            {
                auto peer = static_cast<BonjourPeer*>(ctx);
                sProvider->getAddrResult(flags, interface, err, hostname, address, ttl, peer);
            };

            auto getAddrRef = _serviceRef;
            err = DNSServiceGetAddrInfo(&getAddrRef,
                                             kDNSServiceFlagsShareConnection,
                                             peer->_interface,
                                             kDNSServiceProtocol_IPv4, // could use 0 to get ipv4 & ipv6
                                             hostname,
                                             callback,
                                             peer);
            if (err == 0)
                peer->_getAddrRef = getAddrRef;
            else
                peer->resolveFailed(err);
        }


        // completion routine of DNSServiceGetAddrInfo
        void getAddrResult(DNSServiceFlags flags,
                           uint32_t interface,
                           DNSServiceErrorType err,
                           const char *hostname,
                           const struct sockaddr *address,
                           uint32_t ttl,
                           BonjourPeer* peer) {
            freeServiceRef(peer->_getAddrRef);
            if (err == 0) {
                logInfo("flags=%04x; got IP address of '%s' (ttl=%d)", flags, hostname, ttl);
                peer->resolved(*address, ttl);
            } else {
                peer->resolveFailed(err);
            }
        }


        //---- Service registration / advertising:


        /// API request to register/advertise a service.
        void registerService(uint16_t port, slice txtRecord) {
            if (!_serviceRef) return;
            Assert(_registerRef == nullptr);
            Assert(port != 0);
            logInfo("registering my service '%s' on port %d", _myName.c_str(), port);
            auto regCallback = [](DNSServiceRef,
                                  DNSServiceFlags flags,
                                  DNSServiceErrorType err,
                                  const char* name,
                                  const char* regtype,
                                  const char* domain,
                                  void* ctx) {
                static_cast<BonjourProvider*>(ctx)->regResult(flags, err, name, domain);
            };

            auto registerRef = _serviceRef;
            auto err = DNSServiceRegister(&registerRef,
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
            if (err == 0)
                _registerRef = registerRef;
            else
                ; //FIXME
        }


        /// API request to unregister my service.
        void unregisterService() {
            if (_registerRef) {
                logInfo("unregistering my service '%s'", _myName.c_str());
                freeServiceRef(_registerRef);
            }
        }


        /// API request to update my service's metadata.
        void updateTxtRecord(slice txtRecord) {
            if (_registerRef) {
                // TODO
            }
        }


        void regResult(DNSServiceFlags flags,
                       DNSServiceErrorType err,
                       const char* serviceName,
                       const char* domain)
        {
            if (err) {
                logError("registration error %d", err);
                //TODO: Detect name conflict and retry, appending number to name
                stop();
            } else if (flags & kDNSServiceFlagsAdd) {
                logInfo("flags=%04x; Registered '%s' in %s", flags, serviceName, domain);
            } else {
                logInfo("flags=%04x; Lost registration '%s'", flags, serviceName);
            }
        }

        dispatch_queue_t const queue;          // Dispatch queue I run on

    private:
        DNSServiceRef       _serviceRef {};     // Main connection to dns_sd services
        DNSServiceRef       _browseRef {};      // Secondary connection for browsing peers
        DNSServiceRef       _registerRef {};    // Secondary connection for registring my peer
        string              _myName;            // Name of my service
        uint16_t            _myPort {};         // IP port of my service
        alloc_slice         _myTxtRecord;       // My encoded TXT record
    };


    void InitializeBonjourProvider(string_view serviceType) {
        if (!C4PeerDiscoveryProvider::startBrowsing) {
            sServiceType = serviceType;
            C4PeerDiscoveryProvider::startBrowsing = []() {
                if (!sProvider)
                    sProvider = new BonjourProvider();
                dispatch_async(sProvider->queue, ^{sProvider->start();});
            };
            C4PeerDiscoveryProvider::stopBrowsing = []() {
                if (sProvider)
                    dispatch_async(sProvider->queue, ^{sProvider->stop();});
            };
            C4PeerDiscoveryProvider::monitorMetadata = [](C4Peer* peer, bool start) {
                Retained<C4Peer> retainedPeer(peer);
                dispatch_async(sProvider->queue, ^{
                    if (start)
                        sProvider->monitorTxtRecord(retainedPeer);
                    else
                        sProvider->stopMonitoringTxtRecord(retainedPeer);
                });
            };
            C4PeerDiscoveryProvider::resolveAddresses = [](C4Peer* peer) {
                Retained<C4Peer> retainedPeer(peer);
                dispatch_async(sProvider->queue, ^{sProvider->resolveAddress(retainedPeer);});
            };
        }
    }

}

#endif //___APPLE__
