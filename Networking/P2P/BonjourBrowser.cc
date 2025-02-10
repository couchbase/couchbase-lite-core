//
// Created by Jens Alfke on 2/3/25.
//

#ifdef __APPLE__

#include "BonjourBrowser.hh"
#include "Error.hh"
#include "NetworkInterfaces.hh"
#include "StringUtil.hh"
#include <dns_sd.h>

namespace litecore::p2p {
    using namespace std;


#pragma mark - BONJOUR PEER:


    class BonjourPeer : public Peer {
    public:
        friend struct BonjourBrowser::Impl;

        BonjourPeer(BonjourBrowser* browser, string name, int interface, string domain)
        :Peer(browser, std::move(name))
        ,_domain(std::move(domain))
        ,_interface(interface)
        { }

        BonjourBrowser& browser() {return dynamic_cast<BonjourBrowser&>(*Peer::browser());}

        void resolved(struct sockaddr const* ap, int ttl) {
            IPAddress address(*ap);
            address.setPort(_port);
            setAddress(&address, c4_now() + 1000 * ttl);
        }

        void resolveFailed() {
            setAddress(nullptr);
        }

        bool setTxtRecord(slice txt) {
            unique_lock lock(_mutex);
            if (txt.size == 1 && txt[0] == 0)
                txt = nullslice;
            if (txt == _txtRecord)
                return false;
            _txtRecord = txt;
            return true;
        }

        alloc_slice getMetadata(std::string_view key) const override {
            unique_lock lock(_mutex);
            if (_txtRecord) {
                uint8_t len;
                const void* val = TXTRecordGetValuePtr(uint16_t(_txtRecord.size), _txtRecord.buf,
                                                        string(key).c_str(), &len);
                if (val)
                    return alloc_slice(val, len);
            }
            return nullslice;
        }

        unordered_map<string, alloc_slice> getAllMetadata() const override {
            unique_lock lock(_mutex);
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
            return metadata;
        }

        void removed() {
            unique_lock lock(_mutex);
            if (_monitorTxtRef)
                DNSServiceRefDeallocate(_monitorTxtRef);
            if (_resolveRef)
                DNSServiceRefDeallocate(_resolveRef);
            if (_getAddrRef)
                DNSServiceRefDeallocate(_getAddrRef);
            _port = 0;
            _txtRecord = nullptr;
        }

        string          _domain;
        DNSServiceRef   _monitorTxtRef {};  // reference to DNSServiceQueryRecord task
        DNSServiceRef   _resolveRef {};     // reference to DNSServiceResolve task
        DNSServiceRef   _getAddrRef {};     // reference to DNSServiceGetAddrInfo task
        alloc_slice     _txtRecord;
        int             _interface {};
        uint16_t        _port {};
    };


#pragma mark - BROWSER IMPL:


    /// Internal implementation class of BonjourBrowser.
    /// This class owns a dispatch queue, and all calls other than constructor/destructor
    /// must be made on that queue.
    struct BonjourBrowser::Impl {
        explicit Impl(BonjourBrowser *owner)
        :_owner(owner)
        ,_queue(dispatch_queue_create("P2P Browser", DISPATCH_QUEUE_SERIAL))
        { }

        ~Impl() {
            if (_serviceRef) {
                Warn("Browser was not stopped before deallocating!");
                DNSServiceRefDeallocate(_serviceRef);
            }
            dispatch_release(_queue);
        }


        void check(DNSServiceErrorType err) const {
            if (err) {
                _owner->logError("got error %d", err);
                error(error::Network, 999, stringprintf("DNSServiceError %d", err))._throw(1);
            }
        }

        bool running() const {return _serviceRef != nullptr;}


        void start() {
            if (_serviceRef) return;

            try {
                _owner->_logInfo("browsing '%s'...", _owner->_serviceType.c_str());
                _selfRetain = _owner;

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
                    reinterpret_cast<Impl*>(ctx)->browseResult(flags, err, interface, serviceName, domain);
                };

                _browseRef = _serviceRef;
                check(DNSServiceBrowse(&_browseRef,
                                       kDNSServiceFlagsShareConnection | kDNSServiceFlagsIncludeP2P,
                                       kDNSServiceInterfaceIndexAny,
                                       _owner->_serviceType.c_str(),
                                       nullptr,
                                       browseCallback,
                                       this));

                if (auto port = _owner->myPort())
                    registerService(port, _owner->myTxtRecord());
            } catch (...) {
                if (_serviceRef) {
                    stop();
                } else {
                    _owner->notify(BrowserStopped, nullptr); // notify even if ref wasn't created
                    _selfRetain = nullptr;
                }
            }
        }


        void stop() {
            if (_serviceRef) {
                _owner->_logInfo("stopping");
                DNSServiceRefDeallocate(_serviceRef);
                _serviceRef = _browseRef = _registerRef = nullptr;   // closing main ref closes shared refs too
                _owner->notify(BrowserStopped, nullptr);
                _selfRetain = nullptr;
            }
        }


        void browseResult(DNSServiceFlags flags,
                          DNSServiceErrorType errorCode,
                          int interface,
                          const char *serviceName,
                          const char *domain)
        {
            if (errorCode) {
                _owner->logError("browse error %d", errorCode);
                stop();
            } else if (string_view(serviceName) == _owner->_myName) {
                _owner->_logVerbose("flags=%04x; found echo of my service '%s' in %s", flags, serviceName, domain);
            } else if (flags & kDNSServiceFlagsAdd) {
                _owner->_logInfo("flags=%04x; found '%s' in %s", flags, serviceName, domain);
                auto peer = make_retained<BonjourPeer>(_owner, serviceName, interface, domain);
                (void)_owner->addPeer(peer);
            } else {
                _owner->_logInfo("flags=%04x; lost '%s'", flags, serviceName);
                auto peer = _owner->peerNamed(serviceName);
                if (auto bonjourPeer = dynamic_cast<BonjourPeer*>(peer.get()))
                    bonjourPeer->removed();
                _owner->removePeer(serviceName);
            }
        }


        //---- Monitoring TXT records:


        void monitorTxtRecord(Retained<BonjourPeer> peer) {
            if (peer->_monitorTxtRef)
                return;
            string fullName = stringprintf("%s.%s.%s", peer->name().c_str(), _owner->_serviceType.c_str(), "local");
            _owner->_logInfo("monitoring TXT record of '%s'", fullName.c_str());

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
                auto impl = peer->browser()._impl.get();
                impl->monitorTxtResult(flags, err, slice(rdata, rdlen), ttl, peer);
            };

            peer->_monitorTxtRef = _serviceRef;
            auto err = DNSServiceQueryRecord(&peer->_monitorTxtRef,
                                             kDNSServiceFlagsShareConnection,
                                             kDNSServiceInterfaceIndexAny,
                                             fullName.c_str(),
                                             kDNSServiceType_TXT,
                                             kDNSServiceClass_IN,
                                             callback,
                                             peer);
            Assert(!err);//TEMP
        }


        void stopMonitoringTxtRecord(Retained<BonjourPeer> peer) {
            if (peer->_monitorTxtRef) {
                _owner->_logInfo("stopped monitoring TXT record of '%s'", peer->name().c_str());
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
                _owner->_logInfo("flags=%04x; received TXT of %s (%zu bytes; ttl %d)",
                    flags, peer->name().c_str(), txtRecord.size, ttl);
                if (peer->setTxtRecord(txtRecord))
                    _owner->notify(PeerTxtChanged, peer);
            } else {
                _owner->logError("error %d monitoring TXT record of %s", err, peer->name().c_str());
            }
            // leave the monitoring task running.
        }


        //---- Resolving peer addresses:


        void resolveAddress(Retained<BonjourPeer> peer) {
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
                auto impl = peer->browser()._impl.get();
                impl->resolveResult(flags, err, fullname, hostname,
                                    ntohs(portBE), slice(txtRecord, txtLen), peer);
            };

            peer->_resolveRef = _serviceRef;
            auto err = DNSServiceResolve(&peer->_resolveRef,
                                         kDNSServiceFlagsShareConnection,
                                         peer->_interface,
                                         peer->name().c_str(),
                                         _owner->_serviceType.c_str(),
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
                peer->resolveFailed();
                _owner->notify(PeerResolveFailed, peer);
                return;
            }

            peer->_port = port;
            _owner->_logInfo("flags=%04x; resolved '%s' as hostname=%s, port=%d",
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
                auto impl = peer->browser()._impl.get();
                impl->getAddrResult(flags, interface, err, hostname, address, ttl, peer);
            };
            peer->_getAddrRef = _serviceRef;
            if (peer->setTxtRecord(txtRecord))
                _owner->notify(PeerTxtChanged, peer);

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
                peer->resolveFailed();
                _owner->notify(PeerResolveFailed, peer);
            } else {
                _owner->_logInfo("flags=%04x; got IP address of '%s' (ttl=%d)", flags, hostname, ttl);
                peer->resolved(address, ttl);
                _owner->notify(PeerAddressResolved, peer);
            }
        }


        //---- Service registration / advertising:


        void registerService(uint16_t port, slice txtRecord) {
            if (!_serviceRef) return;
            Assert(_registerRef == nullptr);
            Assert(port != 0);
            _owner->_logInfo("registering my service '%s' on port %d", _owner->_myName.c_str(), port);
            auto regCallback = [](DNSServiceRef,
                                  DNSServiceFlags flags,
                                  DNSServiceErrorType errorCode,
                                  const char* name,
                                  const char* regtype,
                                  const char* domain,
                                  void* ctx) {
                reinterpret_cast<Impl*>(ctx)->regResult(flags, errorCode, name, domain);
            };

            _registerRef = _serviceRef;
            auto err = DNSServiceRegister(&_registerRef,
                                          kDNSServiceFlagsShareConnection |
                                          kDNSServiceFlagsNoAutoRename,
                                          kDNSServiceInterfaceIndexAny,
                                          _owner->_myName.c_str(),
                                          _owner->_serviceType.c_str(),
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
                _owner->_logInfo("unregistering my service '%s'", _owner->_myName.c_str());
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
                _owner->logError("registration error %d", errorCode);
                //TODO: Detect name conflict and retry, appending number to name
                stop();
            } else if (flags & kDNSServiceFlagsAdd) {
                _owner->_logInfo("flags=%04x; Registered '%s' in %s", flags, serviceName, domain);
            } else {
                _owner->_logInfo("flags=%04x; Lost registration '%s'", flags, serviceName);
            }
        }


        BonjourBrowser*             _owner;
        dispatch_queue_t            _queue {};
        DNSServiceRef               _serviceRef {}, _browseRef {}, _registerRef {};
        Retained<BonjourBrowser>    _selfRetain;
        bool                        _stopping {};
    };


#pragma mark - BROWSER:


    BonjourBrowser::BonjourBrowser(string_view serviceType, string_view myName, Observer obs)
    :Browser(serviceType, myName, std::move(obs))
    ,_impl(new Impl(this))
    { }

    void BonjourBrowser::start() {dispatch_async(_impl->_queue, ^{_impl->start();});}

    void BonjourBrowser::stop() {dispatch_async(_impl->_queue, ^{_impl->stop();});}

    void BonjourBrowser::startMonitoring(Peer* peer) {
        Retained<BonjourPeer> rp(dynamic_cast<BonjourPeer*>(peer));
        dispatch_async(_impl->_queue, ^{_impl->monitorTxtRecord(rp);});
    }

    void BonjourBrowser::stopMonitoring(Peer* peer) {
        Retained<BonjourPeer> rp(dynamic_cast<BonjourPeer*>(peer));
        dispatch_async(_impl->_queue, ^{_impl->stopMonitoringTxtRecord(rp);});
    }

    void BonjourBrowser::resolveAddress(Peer* peer) {
        Retained<BonjourPeer> rp(dynamic_cast<BonjourPeer*>(peer));
        dispatch_async(_impl->_queue, ^{_impl->resolveAddress(rp);});
    }


    void BonjourBrowser::setMyPort(uint16_t port) {
        Browser::setMyPort(port);
        dispatch_async(_impl->_queue, ^{
            _impl->unregisterService();
            if (port)
                _impl->registerService(port, this->myTxtRecord());
        });
    }


    void BonjourBrowser::setMyTxtRecord(alloc_slice txt) {
        Browser::setMyTxtRecord(txt);
        dispatch_async(_impl->_queue, ^{
            _impl->updateTxtRecord(txt);
        });
    }
}

#endif //___APPLE__
