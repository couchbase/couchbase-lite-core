//
// Created by Jens Alfke on 2/3/25.
//

#pragma once
#include "Base.hh"
#include "Logging.hh"
#include <functional>
#include <mutex>
#include <vector>

namespace litecore::p2p {
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
            PeerTxtChanged,
        };

        static const char* const kEventNames[];

        using Observer = std::function<void(Browser&, Event, Peer*)>;

        explicit Browser(string_view serviceName, Observer obs);

        virtual void start() = 0;
        virtual void stop() = 0;

        Retained<Peer> peerNamed(string const& name);

    protected:

        void notify(Event event, Peer* peer);

        void addPeer(Retained<Peer> peer);
        void removePeer(string const& name);

        string const _serviceName;
        Observer const _observer;
        std::mutex                  _mutex;
        std::unordered_map<string,Retained<Peer>> _peers;
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

        /// List of known addresses to connect to the peer.
        /// Format of the string is protocol-dependent (e.g. numeric IPv4/IPv6)
        virtual std::vector<std::string> addresses() const =0;

        /// Returns metadata associated with a key string (e.g. from an mDNS TXT record.)
        virtual std::string getMetadata(std::string_view key) const {return "";}

    private:
        Browser* const      _browser;
        string const        _name;
        std::mutex mutable  _mutex;
        alloc_slice         _txtRecord;
    };

}  // namespace litecore::p2p
