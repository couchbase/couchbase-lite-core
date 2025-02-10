//
// Created by Jens Alfke on 2/3/25.
//

#pragma once
#include "Base.hh"
#include "Logging.hh"
#include "NetworkInterfaces.hh"
#include <functional>
#include <mutex>
#include <vector>

namespace litecore::p2p {
    using IPAddress = net::IPAddress;

    class Peer;

    extern LogDomain P2PLog;


    /** A service-discovery browser that locates peers advertising a matching service.
     *  This is an abstract base class. Each platform and discovery protocol will subclass this. */
    class Browser : public RefCounted, public Logging {
      public:
        enum Event {
            BrowserOnline,
            BrowserOffline,
            BrowserStopped,
            PeerAdded,
            PeerRemoved,
            PeerAddressResolved,
            PeerResolveFailed,
            PeerTxtChanged,
        };

        static const char* const kEventNames[];

        using Observer = std::function<void(Browser&, Event, Peer*)>;

        explicit Browser(string_view serviceType, string_view myName, Observer obs);

        virtual void start() = 0;
        virtual void stop() = 0;

        uint16_t myPort() const;
        alloc_slice myTxtRecord() const;

        virtual void setMyPort(uint16_t);
        virtual void setMyTxtRecord(alloc_slice);

        Retained<Peer> peerNamed(string const& name);

        virtual void resolveAddress(Peer*) = 0;
        virtual void startMonitoring(Peer*) = 0;
        virtual void stopMonitoring(Peer*) = 0;

    protected:
        void notify(Event event, Peer* peer);
        [[nodiscard]] bool addPeer(Retained<Peer> peer);
        void removePeer(string const& name);

        string const                                _serviceType;
        string const                                _myName;
        Observer const                              _observer;
        std::mutex mutable                          _mutex;
    private:
        uint16_t                                    _myPort = 0;
        alloc_slice                                 _myTxtRecord;
        std::unordered_map<string,Retained<Peer>>   _peers;
    };


    /** A network peer discovered by a Browser.
     *  (Abstract base class; each Browser subclass will probably have an associated Peer class.) */
    class Peer : public RefCounted {
    public:
        Peer(Browser* browser, string name) :_browser(browser), _name(std::move(name)) {}

        /// Owning Browser.
        Browser* browser() const    {return _browser;}

        /// Peer name.
        string const& name() const  {return _name;}

        /// Request to asynchronously determine the peer's address.
        void resolveAddress()       {_browser->resolveAddress(this);}

        /// Peer's address, if known and if it hasn't passed its time-to-live.
        /// @note Addresses come with a time-to-live and will expire after a time, requiring
        ///   another resolve. You should always be prepared for this method to return nullopt.
        std::optional<IPAddress> address() const;

        void startMonitoring() {_browser->startMonitoring(this);}
        void stopMonitoring() {_browser->stopMonitoring(this);}

        /// Returns metadata associated with a key string (e.g. from an mDNS TXT record.)
        virtual alloc_slice getMetadata(string_view key) const {return nullslice;}

        virtual std::unordered_map<string,alloc_slice> getAllMetadata() const {return {};}

    protected:
        void setAddress(IPAddress const*, C4Timestamp expiration ={});

        std::mutex mutable          _mutex;
    private:
        Browser* const              _browser;
        string const                _name;
        std::optional<IPAddress>    _address;
        C4Timestamp                 _addressExpiration {};
    };

}  // namespace litecore::p2p
