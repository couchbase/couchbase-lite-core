//
// Created by Jens Alfke on 2/3/25.
//

#ifdef __APPLE__

#    include "PeerDiscovery+AppleDNSSD.hh"
#    include "Error.hh"
#    include "Logging.hh"
#    include "NetworkInterfaces.hh"
#    include "StringUtil.hh"
#    include <arpa/inet.h>
#    include <dns_sd.h>
#    include <netinet/in.h>

namespace litecore::p2p {
    using namespace std;

    struct BonjourProvider;

    extern LogDomain P2PLog;
    LogDomain P2PLog("P2P");  //FIXME: Should be cross platform

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

            unordered_map<string, alloc_slice> metadata;
            if ( _txtRecord ) {
                auto     txtLen = uint16_t(_txtRecord.size);
                unsigned count  = TXTRecordGetCount(txtLen, _txtRecord.buf);
                char     key[256];
                for ( unsigned i = 0; i < count; i++ ) {
                    uint8_t     valueLen;
                    const void* value;
                    auto err = TXTRecordGetItemAtIndex(txtLen, _txtRecord.buf, uint16_t(i), sizeof(key), key, &valueLen,
                                                       &value);
                    if ( err ) break;
                    metadata.emplace(key, alloc_slice(value, valueLen));
                }
            }
            this->setMetadata(std::move(metadata));
            return true;
        }

        void resolved(sockaddr const& addr, uint32_t ttl) {
            char   buf[INET6_ADDRSTRLEN];
            string str;
            if ( addr.sa_family == AF_INET ) {
                inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in const&>(addr).sin_addr, buf, sizeof(buf));
                str = stringprintf("%s:%d", buf, _port);
            } else {
                inet_ntop(AF_INET6, &reinterpret_cast<sockaddr_in6 const&>(addr).sin6_addr, buf, sizeof(buf));
                str = stringprintf("[%s]:%d", buf, _port);
            }
            C4PeerAddress address{.address = std::move(str), .expiration = c4_now() + 1000LL * ttl};
            this->setAddresses({&address, 1});
        }

        void resolveFailed(DNSServiceErrorType err) { this->setAddresses({}, convertErrorCode(err)); }

        void removed() override {
            freeServiceRef(_monitorTxtRef);
            freeServiceRef(_resolveRef);
            freeServiceRef(_getAddrRef);
        }

        // Instance data is not protected by a mutex; all calls are made on a single dispatch queue.
        string        _domain;
        uint32_t      _interface{};
        uint16_t      _port{};
        alloc_slice   _txtRecord;
        DNSServiceRef _monitorTxtRef{};  // reference to DNSServiceQueryRecord task
        DNSServiceRef _resolveRef{};     // reference to DNSServiceResolve task
        DNSServiceRef _getAddrRef{};     // reference to DNSServiceGetAddrInfo task
    };

#    pragma mark - BROWSER IMPL:

    /// Implements DNS-SD peer discovery.
    /// This class owns a dispatch queue, and all calls other than constructor/destructor
    /// must be made on that queue.
    struct BonjourProvider
        : public C4PeerDiscoveryProvider
        , public Logging {
        explicit BonjourProvider(string_view serviceType)
            : C4PeerDiscoveryProvider("Bonjour")
            , Logging(P2PLog)
            , _queue(dispatch_queue_create("LiteCore P2P", DISPATCH_QUEUE_SERIAL))
            , _serviceType(stringprintf("_%s._tcp", string(serviceType).c_str())) {
            bool ok = true;
            for (char c : serviceType)
                if (!isalnum(uint8_t(c)) && c != '-') ok = false;
            if (!ok || serviceType.empty() || serviceType.size() > 15)
                error::_throw(error::InvalidParameter, "invalid service type");
        }

        ~BonjourProvider() override {
            if ( _serviceRef ) {
                Warn("Browser was not stopped before deallocating!");
                freeServiceRef(_serviceRef);
            }
            dispatch_release(_queue);
        }

        bool running() const { return _serviceRef != nullptr; }

        void startBrowsing() override {
            dispatch_async(_queue, ^{ do_start(); });
        }

        /// Provider callback that stops browsing for peers. */
        void stopBrowsing() override {
            dispatch_async(_queue, ^{ do_stop(); });
        }

        /// Provider callback that starts or stops monitoring the metadata of a peer.
        void monitorMetadata(C4Peer* peer, bool start) override {
            Retained<BonjourPeer> bonjourPeer(dynamic_cast<BonjourPeer*>(peer));
            dispatch_async(_queue, ^{ do_monitor(bonjourPeer, start); });
        }

        /// Provider callback that requests addresses be resolved for a peer.
        /// This is a one-shot operation. The provider should call `resolvedAddresses` when done.
        void resolveAddresses(C4Peer* peer) override {
            Retained<BonjourPeer> bonjourPeer(dynamic_cast<BonjourPeer*>(peer));
            dispatch_async(_queue, ^{ do_resolve(bonjourPeer); });
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
            } else if ( string_view(serviceName) == _myName ) {
                logVerbose("flags=%04x; found echo of my service '%s' in %s", flags, serviceName, domain);
            } else if ( flags & kDNSServiceFlagsAdd ) {
                logInfo("flags=%04x; found '%s' in %s", flags, serviceName, domain);
                auto peer =
                        make_retained<BonjourPeer>(this, makeID(serviceName, domain), serviceName, interface, domain);
                C4PeerDiscoveryProvider::addPeer(peer);
            } else {
                logInfo("flags=%04x; lost '%s'", flags, serviceName);
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
                auto err           = DNSServiceQueryRecord(&monitorTxtRef, kDNSServiceFlagsShareConnection | kDNSServiceFlagsIncludeP2P,
                                                           peer->_interface, peer->id.c_str(), kDNSServiceType_TXT,
                                                           kDNSServiceClass_IN, callback, peer);
                if ( err == 0 ) {
                    peer->_monitorTxtRef = monitorTxtRef;
                } else {
                    warn("failed to monitor TXT record: err %d", err);
                }
            } else {
                if ( peer->_monitorTxtRef ) {
                    logInfo("stopped monitoring TXT record of '%s'", peer->displayName.c_str());
                    freeServiceRef(peer->_monitorTxtRef);
                }
            }
        }

        void monitorTxtResult(DNSServiceFlags flags, DNSServiceErrorType err, slice txtRecord, uint32_t ttl,
                              BonjourPeer* peer) {
            if ( err == 0 ) {
                logInfo("flags=%04x; received TXT of %s (%zu bytes; ttl %d)", flags, peer->displayName.c_str(),
                        txtRecord.size, ttl);
                peer->setTxtRecord(txtRecord);
            } else {
                logError("error %d monitoring TXT record of %s", err, peer->displayName.c_str());
            }
            // leave the monitoring task running.
        }

        //---- Resolving peer addresses:

        /// API request to resolve a peer's address.
        void do_resolve(Retained<BonjourPeer> peer) {
            // This has to be done in two asynchronous steps:
            // 1. call DNSServiceResolve to get the hostname and port.
            // 2. call DNSServiceGetAddrInfo with the hostname to get the address.

            if ( peer->_resolveRef || peer->_getAddrRef ) return;  // already resolving

            auto callback = [](DNSServiceRef, DNSServiceFlags flags, uint32_t /*interfaceIndex*/,
                               DNSServiceErrorType err, const char* fullname, const char* hostname,
                               uint16_t portBE,  // In network byte order
                               uint16_t txtLen, const unsigned char* txtRecord, void* ctx) {
                auto peer = static_cast<BonjourPeer*>(ctx);
                sProvider->resolveResult(flags, err, fullname, hostname, ntohs(portBE), slice(txtRecord, txtLen), peer);
            };

            auto resolveRef = _serviceRef;
            auto err        = DNSServiceResolve(&resolveRef, kDNSServiceFlagsShareConnection, peer->_interface,
                                                peer->displayName.c_str(), _serviceType.c_str(), peer->_domain.c_str(),
                                                callback, peer);
            if ( err == 0 ) {
                peer->_resolveRef = resolveRef;
            } else {
                peer->resolveFailed(err);
            }
        }

        // completion routine of DNSServiceResolve
        void resolveResult(DNSServiceFlags flags, DNSServiceErrorType err, const char* fullname, const char* hostname,
                           uint16_t port, slice txtRecord, BonjourPeer* peer) {
            freeServiceRef(peer->_resolveRef);
            if ( err ) {
                peer->resolveFailed(err);
                return;
            }

            peer->_port = port;
            logInfo("flags=%04x; resolved '%s' as hostname=%s, port=%d", flags, fullname, hostname, port);

            peer->setTxtRecord(txtRecord);

            auto callback = [](DNSServiceRef, DNSServiceFlags flags, uint32_t /*interface*/, DNSServiceErrorType err,
                               const char* hostname, const sockaddr* address, uint32_t ttl, void* ctx) {
                auto peer = static_cast<BonjourPeer*>(ctx);
                sProvider->getAddrResult(flags, err, hostname, address, ttl, peer);
            };

            auto getAddrRef = _serviceRef;
            err             = DNSServiceGetAddrInfo(&getAddrRef, kDNSServiceFlagsShareConnection, peer->_interface,
                                                    kDNSServiceProtocol_IPv4,  // could use 0 to get ipv4 & ipv6
                                                    hostname, callback, peer);
            if ( err == 0 ) peer->_getAddrRef = getAddrRef;
            else
                peer->resolveFailed(err);
        }

        // completion routine of DNSServiceGetAddrInfo
        void getAddrResult(DNSServiceFlags flags, DNSServiceErrorType err, const char* hostname,
                           const sockaddr* address, uint32_t ttl, BonjourPeer* peer) {
            freeServiceRef(peer->_getAddrRef);
            if ( err == 0 ) {
                logInfo("flags=%04x; got IP address of '%s' (ttl=%d)", flags, hostname, ttl);
                peer->resolved(*address, ttl);
            } else {
                peer->resolveFailed(err);
            }
        }

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

            publishStateChanged(err == 0, convertErrorCode(err));
        }

        DNSServiceErrorType republish() {
            Assert(!_registerRef);
            if ( _myDupCount == 0 ) _myName = _myBaseName;
            else
                _myName = stringprintf("%s %u", _myBaseName.c_str(), _myDupCount + 1);
            logInfo("publishing my service '%s' on port %d", _myName.c_str(), _myPort);
            auto regCallback = [](DNSServiceRef, DNSServiceFlags flags, DNSServiceErrorType err, const char* name,
                                  const char* /*regtype*/, const char* domain, void* ctx) {
                static_cast<BonjourProvider*>(ctx)->regResult(flags, err, name, domain);
            };

            auto registerRef = _serviceRef;
            auto err = DNSServiceRegister(&registerRef, kDNSServiceFlagsShareConnection | kDNSServiceFlagsNoAutoRename,
                                          kDNSServiceInterfaceIndexAny, _myName.c_str(), _serviceType.c_str(), nullptr,
                                          nullptr, htons(_myPort), uint16_t(_myTxtRecord.size), _myTxtRecord.buf, regCallback,
                                          this);
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
                logInfo("flags=%04x; Registered '%s' in %s", flags, serviceName, domain);
            } else {
                logInfo("flags=%04x; Lost registration '%s'", flags, serviceName);
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
            TXTRecordRef txtRef;
            TXTRecordCreate(&txtRef, 0, nullptr);
            DNSServiceErrorType err = 0;
            for ( auto& [key, value] : meta ) {
                if ( value.size > 0xFF ) {
                    logError("setMyTxtRecord: value of '%s' is too long, %zu bytes", key.c_str(), value.size);
                    err = kDNSServiceErr_BadParam;
                    break;
                }
                err = TXTRecordSetValue(&txtRef, key.c_str(), uint8_t(value.size), value.buf);
                if ( err ) {
                    logError("setMyTxtRecord: error %d adding key '%s'", err, key.c_str());
                    break;
                }
            }
            if ( err == 0 ) _myTxtRecord = alloc_slice(TXTRecordGetBytesPtr(&txtRef), TXTRecordGetLength(&txtRef));
            TXTRecordDeallocate(&txtRef);
            return err;
        }

      private:
        dispatch_queue_t const _queue;           // Dispatch queue I run on
        string const           _serviceType;     // DNS-SD service name
        DNSServiceRef          _serviceRef{};    // Main connection to dns_sd services
        DNSServiceRef          _browseRef{};     // Secondary connection for browsing peers
        DNSServiceRef          _registerRef{};   // Secondary connection for registring my peer
        string                 _myBaseName;      // Name of my service, as given by client
        string                 _myName;          // Actual published name of my service
        unsigned               _myDupCount = 0;  // Counter to append to _myName when > 0
        uint16_t               _myPort;          // Port number of my service
        alloc_slice            _myTxtRecord;     // My encoded TXT record
    };

    void InitializeBonjourProvider(string_view serviceType) {
        static once_flag sOnce;
        call_once(sOnce, [&] {
            sProvider = new BonjourProvider(serviceType);
            C4PeerDiscovery::registerProvider(sProvider);
        });
    }

}  // namespace litecore::p2p

#endif  //___APPLE__
