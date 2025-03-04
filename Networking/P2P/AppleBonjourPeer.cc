//
// PeerDiscover+AppleDNSSD.cc
//
// Copyright 2025-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#ifdef __APPLE__

#    include "AppleBonjourPeer.hh"
#    include "Address.hh"
#    include "Error.hh"
#    include "Logging.hh"
#    include "NetworkInterfaces.hh"
#    include "StringUtil.hh"
#    include <arpa/inet.h>
#    include <dns_sd.h>
#    include <netinet/in.h>


// If `ADDRESS_IN_URL` is defined, peer URLs will use numeric IP addresses instead of mDNS hostnames,
// e.g. `10.0.1.23` instead of `jens.local.`. I've turned this off because it takes longer to resolve and is
// potentially less robust, i.e. if a peer's address changes. --Jens, March 3 2025
//#define ADDRESS_IN_URL


namespace litecore::p2p {
    using namespace std;

    extern LogDomain P2PLog;

    struct BonjourProvider;

    static BonjourProvider* sProvider;

    static C4Error convertErrorCode(DNSServiceErrorType err) {
        if ( err ) return C4Error::make(NetworkDomain, 999, stringprintf("DNSServiceError %d", err));
        else
            return kC4NoError;
    }

    static void freeServiceRef(DNSServiceRef& ref) {
        if ( ref ) {
            DNSServiceRefDeallocate(ref);
            ref = nullptr;
        }
    }

    alloc_slice EncodeMetadataAsTXT(C4Peer::Metadata const& meta, int* outError) {
        TXTRecordRef txtRef;
        TXTRecordCreate(&txtRef, 0, nullptr);
        DNSServiceErrorType err = 0;
        for ( auto& [key, value] : meta ) {
            if ( value.size > 0xFF ) {
                LogToAt(P2PLog, Error, "EncodeMetadataAsTXT: value of '%s' is too long, %zu bytes", key.c_str(),
                        value.size);
                err = kDNSServiceErr_BadParam;
                break;
            }
            err = TXTRecordSetValue(&txtRef, key.c_str(), uint8_t(value.size), value.buf);
            if ( err ) {
                LogToAt(P2PLog, Error, "EncodeMetadataAsTXT: error %d setting key '%s'", err, key.c_str());
                break;
            }
        }
        alloc_slice result;
        if ( err == 0 ) result = alloc_slice(TXTRecordGetBytesPtr(&txtRef), TXTRecordGetLength(&txtRef));
        else if ( outError )
            *outError = err;
        TXTRecordDeallocate(&txtRef);
        return result;
    }

    C4Peer::Metadata DecodeTXTToMetadata(slice txtRecord) {
        C4Peer::Metadata metadata;
        if ( txtRecord ) {
            if ( txtRecord.size > 0xFFFF ) {
                LogToAt(P2PLog, Error, "DecodeTXTToMetadata: invalid size %zu", txtRecord.size);
                return metadata;
            }
            auto     txtLen = uint16_t(txtRecord.size);
            unsigned count  = TXTRecordGetCount(txtLen, txtRecord.buf);
            char     key[256];
            for ( unsigned i = 0; i < count; i++ ) {
                uint8_t     valueLen;
                const void* value;
                auto err = TXTRecordGetItemAtIndex(txtLen, txtRecord.buf, uint16_t(i), sizeof(key), key, &valueLen,
                                                   &value);
                if ( err ) {
                    LogToAt(P2PLog, Error, "DecodeTXTToMetadata: error %d", err);
                    break;
                }
                metadata.emplace(key, alloc_slice(value, valueLen));
            }
        }
        return metadata;
    }

#    pragma mark - BONJOUR PEER:

    /** C4Peer subclass created by BonjourProvider. */
    class BonjourPeer : public C4Peer {
      public:
        BonjourPeer(C4PeerDiscoveryProvider* provider, string const& id, string const& name, uint32_t interface,
                    string domain)
            : C4Peer(provider, id, name), _domain(std::move(domain)), _interface(interface) {}

        bool setTxtRecord(slice txt) {
            if ( txt.size == 1 && txt[0] == 0 )  // empty record consists of a single 00 byte
                txt = nullslice;
            if ( txt == _txtRecord ) return false;
            _txtRecord = txt;
            this->setMetadata(DecodeTXTToMetadata(_txtRecord));
            return true;
        }

        void gotAddress(sockaddr const& addr, uint32_t ttl) {
            _address           = addr;
            _addressExpiration = c4_now() + 1000LL * ttl;
        }

        void getAddressFailed(DNSServiceErrorType err) {
            _addressExpiration = C4Timestamp(0);
            resolvedURL(nullptr, convertErrorCode(err));
        }

        bool addressValid() { return _addressExpiration > c4_now(); }

        string addressString() {
#ifdef ADDRESS_IN_URL
            if ( !addressValid() ) return "";
            char buf[INET6_ADDRSTRLEN];
            if ( _address.sa_family == AF_INET ) {
                inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in const&>(_address).sin_addr, buf, sizeof(buf));
            } else {
                buf[0] = '[';
                inet_ntop(AF_INET6, &reinterpret_cast<sockaddr_in6 const&>(_address).sin6_addr, &buf[1],
                          sizeof(buf) - 2);
                strlcat(buf, "]", sizeof(buf));
            }
            return buf;
#else
            return _hostname;
#endif
        }

        void removed() override {
            C4Peer::removed();
            _addressExpiration = C4Timestamp(0);
            freeServiceRef(_monitorTxtRef);
            freeServiceRef(_resolveRef);
            freeServiceRef(_getAddrRef);
        }

        // Instance data is not protected by a mutex; all calls are made on a single dispatch queue.
        string        _domain;
        string        _hostname;
        uint32_t      _interface{};
        uint16_t      _port{};
        sockaddr      _address{};
        C4Timestamp   _addressExpiration{};
        alloc_slice   _txtRecord;
        DNSServiceRef _monitorTxtRef{};  // reference to DNSServiceQueryRecord task
        DNSServiceRef _resolveRef{};     // reference to DNSServiceResolve task
        DNSServiceRef _getAddrRef{};     // reference to DNSServiceGetAddrInfo task
    };

#    pragma mark - BROWSER PUBLIC API:

    /// Implements DNS-SD peer discovery.
    /// This class is basically an Actor -- it owns a dispatch queue, and all API calls are forwarded to it.
    struct BonjourProvider
        : public C4PeerDiscoveryProvider
        , public Logging {
        explicit BonjourProvider(string_view serviceType)
            : C4PeerDiscoveryProvider("DNS-SD")
            , Logging(P2PLog)
            , _queue(dispatch_queue_create("LiteCore DNS-SD", DISPATCH_QUEUE_SERIAL))
            , _serviceType(stringprintf("_%s._tcp", string(serviceType).c_str())) {
            bool ok = true;
            for ( char c : serviceType )
                if ( !isalnum(uint8_t(c)) && c != '-' ) ok = false;
            if ( !ok || serviceType.empty() || serviceType.size() > 15 )
                error::_throw(error::InvalidParameter, "invalid service type");
        }

        void startBrowsing() override {
            dispatch_async(_queue, ^{ do_start(); });
        }

        void stopBrowsing() override {
            dispatch_async(_queue, ^{ do_stop(); });
        }

        void monitorMetadata(C4Peer* peer, bool start) override {
            Retained<BonjourPeer> bonjourPeer(dynamic_cast<BonjourPeer*>(peer));
            dispatch_async(_queue, ^{ do_monitor(bonjourPeer, start); });
        }

        void resolveURL(C4Peer* peer) override {
            Retained<BonjourPeer> bonjourPeer(dynamic_cast<BonjourPeer*>(peer));
            dispatch_async(_queue, ^{ do_resolveURL(bonjourPeer); });
        }

        void cancelResolveURL(C4Peer* peer) override {
            Retained<BonjourPeer> bonjourPeer(dynamic_cast<BonjourPeer*>(peer));
            dispatch_async(_queue, ^{ do_cancelResolveURL(bonjourPeer); });
        }

        C4SocketFactory const* C4NULLABLE getSocketFactory() const override { return nullptr; }

        void connect(C4Peer* peer) override {
            Retained<BonjourPeer> bonjourPeer(dynamic_cast<BonjourPeer*>(peer));
            dispatch_async(_queue, ^{ do_connect(bonjourPeer); });
        }

        void cancelConnect(C4Peer* peer) override {
            Retained<BonjourPeer> bonjourPeer(dynamic_cast<BonjourPeer*>(peer));
            dispatch_async(_queue, ^{ do_cancelConnect(bonjourPeer); });
        }

        void publish(std::string_view displayName, uint16_t port, C4Peer::Metadata const& meta) override {
            string           nameStr(displayName);
            C4Peer::Metadata metaCopy = meta;
            dispatch_async(_queue, ^{ do_publish(std::move(nameStr), port, std::move(metaCopy)); });
        }

        void unpublish() override {
            dispatch_async(_queue, ^{ do_unpublish(); });
        }

        void updateMetadata(C4Peer::Metadata const& meta) override {
            C4Peer::Metadata metaCopy = meta;
            dispatch_async(_queue, ^{ do_updateMetadata(std::move(metaCopy)); });
        }

        void shutdown(std::function<void()> onComplete) override {
            dispatch_async(_queue, ^{
                do_stop();
                do_unpublish();
                onComplete();
            });
        }

        ~BonjourProvider() override {
            if (this == sProvider)
                sProvider = nullptr;
            if ( _browseRef || _registerRef )
                warn("Provider was not stopped before deallocating!");
            freeServiceRef(_serviceRef);
            dispatch_release(_queue);
        }

#pragma mark - BROWSER IMPLEMENTATION:

        // From here on, all methods are called on the dispatch queue.

      private:
        string makeID(string const& name, string const& domain) { return name + "." + _serviceType + "." + domain; }

        DNSServiceErrorType openServiceRef() {
            if ( _serviceRef ) return 0;
            auto err = DNSServiceCreateConnection(&_serviceRef);
            if ( err ) return err;
            err = DNSServiceSetDispatchQueue(_serviceRef, _queue);
            if ( err ) freeServiceRef(_serviceRef);
            return err;
        }

        /// API request to start browsing.
        void do_start() {
            if ( _browseRef ) return;

            DNSServiceErrorType err = 0;
            do {
                logInfo("browsing '%s'...", _serviceType.c_str());

                err = openServiceRef();
                if ( err ) break;

                // Start browser:
                auto browseCallback = [](DNSServiceRef, DNSServiceFlags flags, uint32_t interface,
                                         DNSServiceErrorType err, const char* serviceName, const char* /*regtype*/,
                                         const char* domain, void* ctx) -> void {
                    static_cast<BonjourProvider*>(ctx)->browseResult(flags, err, interface, serviceName, domain);
                };

                auto browseRef = _serviceRef;
                err = DNSServiceBrowse(&browseRef, kDNSServiceFlagsShareConnection | kDNSServiceFlagsIncludeP2P,
                                       kDNSServiceInterfaceIndexAny, _serviceType.c_str(), nullptr, browseCallback,
                                       this);
                if ( err ) break;
                _browseRef = browseRef;

                browseStateChanged(true);
            } while ( false );
            if ( err ) do_stop(err);
        }

        /// API request to stop browsing.
        void do_stop(DNSServiceErrorType err = 0) {
            bool opened = (_browseRef != nullptr);
            if ( opened ) {
                logInfo("stopping browsing");
                freeServiceRef(_browseRef);
            }
            if ( opened || err ) browseStateChanged(false, convertErrorCode(err));
        }

        void browseResult(DNSServiceFlags flags, DNSServiceErrorType err, uint32_t interface, const char* serviceName,
                          const char* domain) {
            if ( err ) {
                logError("browse error %d", err);
                do_stop(err);
            } else if ( _published && string_view(serviceName) == _myName ) {
                logVerbose("flags=%04x; found echo of my service '%s' in %s", flags, serviceName, domain);
            } else if ( flags & kDNSServiceFlagsAdd ) {
                logInfo("Adding peer '%s' in %s", serviceName, domain);
                auto peer =
                        make_retained<BonjourPeer>(this, makeID(serviceName, domain), serviceName, interface, domain);
                C4PeerDiscoveryProvider::addPeer(peer);
            } else {
                logInfo("Removing peer '%s' in %s", serviceName, domain);
                C4PeerDiscoveryProvider::removePeer(makeID(serviceName, domain));
            }
        }

        //---- Monitoring TXT records:


        /// API request to monitor a peer's metadata.
        void do_monitor(Retained<BonjourPeer> peer, bool start) {
            if ( start ) {
                if ( peer->_monitorTxtRef ) return;
                logInfo("monitoring TXT record of '%s'", peer->id.c_str());

                auto callback = [](DNSServiceRef, DNSServiceFlags flags, uint32_t /*interfaceIndex*/,
                                   DNSServiceErrorType err, const char* /*fullname*/, uint16_t /*rrtype*/,
                                   uint16_t /*rrclass*/, uint16_t rdlen, const void* rdata, uint32_t ttl,
                                   void* ctx) -> void {
                    auto peer = static_cast<BonjourPeer*>(ctx);
                    sProvider->monitorTxtResult(flags, err, slice(rdata, rdlen), ttl, peer);
                };

                auto monitorTxtRef = _serviceRef;
                auto err           = DNSServiceQueryRecord(
                        &monitorTxtRef, kDNSServiceFlagsShareConnection | kDNSServiceFlagsIncludeP2P, peer->_interface,
                        peer->id.c_str(), kDNSServiceType_TXT, kDNSServiceClass_IN, callback, peer);
                if ( err == 0 ) {
                    peer->_monitorTxtRef = monitorTxtRef;
                } else {
                    warn("failed to monitor TXT record: err %d", err);
                }
            } else {
                if ( peer->_monitorTxtRef ) {
                    logInfo("stopped monitoring TXT record of '%s'", peer->displayName().c_str());
                    freeServiceRef(peer->_monitorTxtRef);
                }
            }
        }

        void monitorTxtResult(DNSServiceFlags flags, DNSServiceErrorType err, slice txtRecord, uint32_t ttl,
                              BonjourPeer* peer) {
            if ( err == 0 ) {
                logInfo("flags=%04x; received TXT of %s (%zu bytes; ttl %d)", flags, peer->displayName().c_str(),
                        txtRecord.size, ttl);
                peer->setTxtRecord(txtRecord);
            } else {
                logError("error %d monitoring TXT record of %s", err, peer->displayName().c_str());
            }
            // leave the monitoring task running.
        }

        //---- Resolving peer addresses and connecting:

        /// API request to connect to a peer.
        void do_resolveURL(Retained<BonjourPeer> peer) {
            // This has to be done in steps:
            // 1. call DNSServiceResolve to get the hostname and port.
            // 2. call DNSServiceGetAddrInfo with the hostname to get the address.

            // If we have a valid cached address, skip resolution:
            if ( peer->addressValid() ) {
                resolvedURL(peer);
                return;
            }

            if ( peer->_resolveRef || peer->_getAddrRef ) return;  // already resolving

            if ( !peer->_hostname.empty() ) {
#ifdef ADDRESS_IN_URL
                getAddress(peer);
#else
                resolvedURL(peer);
#endif
                return;
            }

            logInfo("Resolving hostname/port of peer %s ...", peer->id.c_str());
            auto callback = [](DNSServiceRef, DNSServiceFlags flags, uint32_t /*interfaceIndex*/,
                               DNSServiceErrorType err, const char* fullname, const char* hostname,
                               uint16_t portBE,  // In network byte order
                               uint16_t txtLen, const unsigned char* txtRecord, void* ctx) {
                auto peer = static_cast<BonjourPeer*>(ctx);
                sProvider->resolveResult(flags, err, fullname, hostname, ntohs(portBE), slice(txtRecord, txtLen), peer);
            };

            auto resolveRef = _serviceRef;
            auto err        = DNSServiceResolve(&resolveRef, kDNSServiceFlagsShareConnection, peer->_interface,
                                                peer->displayName().c_str(), _serviceType.c_str(), peer->_domain.c_str(),
                                                callback, peer);
            if ( err == 0 ) {
                peer->_resolveRef = resolveRef;
            } else {
                peer->getAddressFailed(err);
            }
        }

        /// API request to cancel a connection attempt.
        void do_cancelResolveURL(Retained<BonjourPeer> peer) {
            freeServiceRef(peer->_resolveRef);
            freeServiceRef(peer->_getAddrRef);
        }

        // completion routine of DNSServiceResolve
        void resolveResult(DNSServiceFlags flags, DNSServiceErrorType err, const char* fullname, const char* hostname,
                           uint16_t port, slice txtRecord, BonjourPeer* peer) {
            freeServiceRef(peer->_resolveRef);
            if ( err ) {
                peer->getAddressFailed(err);
                return;
            }

            logInfo("flags=%04x; resolved '%s' as hostname=%s, port=%d", flags, fullname, hostname, port);
            peer->_hostname = hostname;
            peer->_port     = port;
            peer->setTxtRecord(txtRecord);

#ifdef ADDRESS_IN_URL
            getAddress(peer);
#else
            resolvedURL(peer);
#endif
        }

#ifdef ADDRESS_IN_URL
        void getAddress(BonjourPeer* peer) {
            logInfo("Getting IP address of peer %s ...", peer->id.c_str());
            Assert(!peer->_hostname.empty());

            auto callback = [](DNSServiceRef, DNSServiceFlags flags, uint32_t /*interface*/, DNSServiceErrorType err,
                               const char* hostname, const sockaddr* address, uint32_t ttl, void* ctx) {
                auto peer = static_cast<BonjourPeer*>(ctx);
                sProvider->getAddrResult(flags, err, hostname, address, ttl, peer);
            };

            auto getAddrRef = _serviceRef;
            auto err        = DNSServiceGetAddrInfo(&getAddrRef, kDNSServiceFlagsShareConnection, peer->_interface,
                                                    kDNSServiceProtocol_IPv4,  // could use 0 to get ipv4 & ipv6
                                                    peer->_hostname.c_str(), callback, peer);
            if ( err == 0 ) peer->_getAddrRef = getAddrRef;
            else
                peer->getAddressFailed(err);
        }

        // completion routine of DNSServiceGetAddrInfo
        void getAddrResult(DNSServiceFlags flags, DNSServiceErrorType err, const char* hostname,
                           const sockaddr* address, uint32_t ttl, BonjourPeer* peer) {
            freeServiceRef(peer->_getAddrRef);
            if ( err == 0 ) {
                logInfo("flags=%04x; got IP address of '%s' (ttl=%d)", flags, hostname, ttl);
                peer->gotAddress(*address, ttl);
                resolvedURL(peer);
            } else {
                peer->getAddressFailed(err);
            }
        }
#endif

        void resolvedURL(BonjourPeer* peer) {
            net::Address addr("wss", peer->addressString(), peer->_port, "/db");  //TODO: Real port, db name
            peer->resolvedURL(string(addr.url()), {});
        }

        void do_connect(Retained<BonjourPeer> peer) {
            peer->connected(nullptr, C4Error{LiteCoreDomain, kC4ErrorUnimplemented});
        }

        void do_cancelConnect(Retained<BonjourPeer> peer) {}

        //---- Service publishing:


        /// API request to register/advertise a service.
        void do_publish(string displayName, uint16_t port, C4Peer::Metadata metadata) {
            if ( _registerRef ) return;
            Assert(!displayName.empty());
            Assert(port != 0);

            DNSServiceErrorType err;
            do {
                err = openServiceRef();
                if ( err ) break;

                _myPort = port;
                if ( displayName != _myBaseName ) {
                    _myBaseName = std::move(displayName);
                    _myDupCount = 0;
                }
                err = encodeMyTxtRecord(metadata);
                if ( err ) break;

                err = republish();
            } while ( false );
            if ( err ) publishStateChanged(false, convertErrorCode(err));
        }

        DNSServiceErrorType republish() {
            Assert(!_registerRef);
            if ( _myDupCount == 0 ) _myName = _myBaseName;
            else
                _myName = stringprintf("%s %u", _myBaseName.c_str(), _myDupCount + 1);
            logVerbose("publishing my service '%s' on port %d", _myName.c_str(), _myPort);
            auto regCallback = [](DNSServiceRef, DNSServiceFlags flags, DNSServiceErrorType err, const char* name,
                                  const char* /*regtype*/, const char* domain, void* ctx) {
                static_cast<BonjourProvider*>(ctx)->regResult(flags, err, name, domain);
            };

            auto registerRef = _serviceRef;
            auto err = DNSServiceRegister(&registerRef, kDNSServiceFlagsShareConnection | kDNSServiceFlagsNoAutoRename,
                                          kDNSServiceInterfaceIndexAny, _myName.c_str(), _serviceType.c_str(), nullptr,
                                          nullptr, htons(_myPort), uint16_t(_myTxtRecord.size), _myTxtRecord.buf,
                                          regCallback, this);
            if ( err == 0 ) _registerRef = registerRef;
            return err;
        }

        void regResult(DNSServiceFlags flags, DNSServiceErrorType err, const char* serviceName, const char* domain) {
            if ( err ) {
                if ( err == kDNSServiceErr_NameConflict && _myDupCount < 100 ) {
                    warn("publish name conflict with %s; retrying...", _myName.c_str());
                    freeServiceRef(_registerRef);
                    _myDupCount++;
                    republish();
                } else {
                    logError("publishing error %d", err);
                    do_unpublish();
                }
            } else if ( flags & kDNSServiceFlagsAdd ) {
                logInfo("Registered my peer '%s' in %s", serviceName, domain);
                _published = true;
                publishStateChanged(true);
            } else {
                logInfo("Unregistered my peer '%s' in %s", serviceName, domain);
                _published = false;
                publishStateChanged(false);
            }
        }

        /// API request to unregister my service.
        void do_unpublish() {
            if ( _registerRef ) {
                logInfo("unpublishing my service '%s'", _myName.c_str());
                freeServiceRef(_registerRef);
                publishStateChanged(false, {});
                _myName     = "";
                _myDupCount = 0;
            }
        }

        /// API request to update my service's metadata.
        void do_updateMetadata(C4Peer::Metadata metadata) {
            if ( _registerRef ) {
                auto err = encodeMyTxtRecord(metadata);
                if ( err == 0 )
                    err = DNSServiceUpdateRecord(_registerRef, nullptr, 0, uint16_t(_myTxtRecord.size),
                                                 _myTxtRecord.buf, 0);
                if ( err ) {
                    logError("error %d updating TXT record", err);
                    // FIXME: Report the error
                }
            }
        }

        // Updates _myTxtRecord from a Metadata map
        DNSServiceErrorType encodeMyTxtRecord(C4Peer::Metadata const& meta) {
            DNSServiceErrorType err = 0;
            auto                txt = EncodeMetadataAsTXT(meta, &err);
            if ( txt ) _myTxtRecord = std::move(txt);
            return err;
        }

      private:
        dispatch_queue_t const _queue;              // Dispatch queue I run on
        string const           _serviceType;        // DNS-SD service name
        DNSServiceRef          _serviceRef{};       // Main connection to dns_sd services
        DNSServiceRef          _browseRef{};        // Secondary connection for browsing peers
        DNSServiceRef          _registerRef{};      // Secondary connection for registring my peer
        string                 _myBaseName;         // Name of my service, as given by client
        string                 _myName;             // Actual published name of my service
        unsigned               _myDupCount = 0;     // Counter to append to _myName when > 0
        uint16_t               _myPort;             // Port number of my service
        alloc_slice            _myTxtRecord;        // My encoded TXT record
        bool                   _published = false;  // True when my service is published
    };

    void InitializeBonjourProvider(string_view serviceType) {
        Assert(!sProvider);
        sProvider = new BonjourProvider(serviceType);
        sProvider->registerProvider();
    }

}  // namespace litecore::p2p

#endif  //___APPLE__
