//
// Created by Jens Alfke on 2/3/25.
//

#ifdef __APPLE__

#include "BonjourBrowser.hh"
#include <dispatch/dispatch.h>
#include <Network/Network.h>

namespace litecore::p2p {
    using namespace std;


    // Smart pointer that handles retain/release for Network and Dispatch objects.
    template <typename T, void* (*Retain)(void*), void (*Release)(void*)>
    class OSRetained {
    public:
        // Constructor assumes you have a ref to the object, so it does not retain it!
        explicit OSRetained(T *obj = nullptr)   : _obj(obj) { }
        OSRetained(OSRetained const& r)         :_obj(r._obj) {Retain(_obj);}
        ~OSRetained()                           {Release(_obj);}
        operator T*() const                     {return _obj;}
        T* operator* () const                   {return _obj;}
        T* operator-> () const                  {return _obj;}

        OSRetained& operator= (T* other) {
            nw_retain(other._obj);
            nw_release(_obj);
            _obj = other;
            return *this;
        }
    private:
        T* _obj;
    };

    static void* xdispatch_retain(void* x) {dispatch_retain((dispatch_object_t)x); return x;}
    static void xdispatch_release(void* x) {dispatch_release((dispatch_object_t)x);}

    template <typename T> using NwRetained =       OSRetained<T, nw_retain, nw_release>;
    template <typename T> using DispatchRetained = OSRetained<T, xdispatch_retain, xdispatch_release>;


#pragma mark - BONJOUR PEER:


    class BonjourPeer : public Peer {
    public:
        BonjourPeer(Browser* browser, string name, nw_endpoint_t endpt)
        :Peer(browser, std::move(name))
        ,endpoint(endpt)
        { }

        nw_endpoint_t const endpoint;

        std::vector<std::string> addresses() const override {
            throw logic_error("UNIMPLEMENTED");//TODO
        }

        void setNwTxtRecord(nw_txt_record_t nwTxt) {
            unique_lock lock(_mutex);
            _nwTxtRecord = nwTxt;
        }

    private:
        mutex mutable               _mutex;
        NwRetained<nw_txt_record>   _nwTxtRecord;
    };


#pragma mark - BROWSER IMPL:


    struct BonjourBrowser::Impl {
        explicit Impl(BonjourBrowser *owner)
        :_owner(owner)
        ,_queue(dispatch_queue_create("P2P Browser", DISPATCH_QUEUE_SERIAL))
        { }

        void start() {
            if (_browser) return;
            _owner->_logInfo("starting...");
            auto descriptor = nw_browse_descriptor_create_bonjour_service(_owner->_serviceName.c_str(), nullptr);
            _browser = nw_browser_create(descriptor, nullptr);
            nw_browser_set_queue(_browser, _queue);
            nw_browser_set_state_changed_handler(_browser,
                                                 ^(nw_browser_state_t state, nw_error_t error) {
                this->stateChanged(state, error);
            });
            nw_browser_set_browse_results_changed_handler(_browser,
                                                          ^(nw_browse_result_t oldResult,
                                                            nw_browse_result_t newResult,
                                                            bool batchComplete) {
                this->resultsChanged(oldResult, newResult, batchComplete);
            });
            nw_browser_start(_browser);
            _selfRetain = _owner;
        }


        void stop() {
            if (_browser && !_stopping) {
                _owner->_logInfo("stopping...");
                _stopping = true;
                nw_browser_cancel(_browser);
                // canceling is asynchronous ... wait for state change to 'canceled'
            }
        }

        void stateChanged(nw_browser_state_t state, nw_error_t error) {
            static constexpr const char* kStateName[] = {"invalid", "ready", "failed", "cancelled", "waiting"};
            if (error) {
                // note:
                _owner->logError("state changed: %s; error domain %d, code %d",
                    kStateName[state], nw_error_get_error_domain(error), nw_error_get_error_code(error));
            } else {
                _owner->_logInfo("state changed: %s", kStateName[state]);
            }
            auto prevState = _state;
            _state = state;
            _error = error;
            switch (state) {
            case nw_browser_state_waiting:
                if (prevState == nw_browser_state_ready)
                    _owner->notify(BrowserOffline, nullptr);
                break;
            case nw_browser_state_ready:
                _owner->notify(BrowserOnline, nullptr);
                break;
            case nw_browser_state_failed:
                _owner->notify(BrowserStopped, nullptr);
                stop(); // cancel browser
                break;
            case nw_browser_state_cancelled:
                if (prevState != nw_browser_state_failed)
                    _owner->notify(BrowserStopped, nullptr);
                _browser = nullptr;
                _stopping = false;
                _selfRetain = nullptr;  // might free me!
                break;
            default:
                break;
            }
        }


        void resultsChanged(nw_browse_result_t oldResult,
                               nw_browse_result_t newResult,
                               bool batchComplete)
        {
            auto changeBits = nw_browse_result_get_changes(oldResult, newResult);
            _owner->_logInfo("results changed: %02x ; old=%p, new=%p", unsigned(changeBits), oldResult, newResult);
            if (changeBits & nw_browse_result_change_identical)
                return;
            nw_endpoint_t endpt;
            if (changeBits & nw_browse_result_change_result_removed)
                endpt = nw_browse_result_copy_endpoint(oldResult);
            else
                endpt = nw_browse_result_copy_endpoint(newResult);
            string name = nw_endpoint_get_bonjour_service_name(endpt);

            if (changeBits & nw_browse_result_change_result_removed) {
                _owner->removePeer(name);
            } else {
                Retained<BonjourPeer> peer;
                if (changeBits & nw_browse_result_change_result_added) {
                    peer = make_retained<BonjourPeer>(_owner, name, endpt);
                    _owner->addPeer(peer);
                } else {
                    peer = dynamic_cast<BonjourPeer*>(_owner->peerNamed(name).get());
                }
                if (changeBits & nw_browse_result_change_txt_record_changed) {
                    _owner->_logInfo("Got peer TXT record");
                    peer->setNwTxtRecord(nw_browse_result_copy_txt_record_object(newResult));
                }
            }

        }

        BonjourBrowser*             _owner;
        DispatchRetained<dispatch_queue_s>            _queue {};
        NwRetained<nw_browser>      _browser {};
        nw_browser_state_t          _state {};
        NwRetained<nw_error>        _error {};
        Retained<BonjourBrowser>    _selfRetain;
        bool                        _stopping {};
    };


#pragma mark - BROWSER:
    

    BonjourBrowser::BonjourBrowser(string_view serviceName, Observer obs)
    :Browser(serviceName, std::move(obs))
    ,_impl(new Impl(this))
    { }

    void BonjourBrowser::start() {dispatch_async(_impl->_queue, ^{_impl->start();});}

    void BonjourBrowser::stop() {dispatch_async(_impl->_queue, ^{_impl->stop();});}

}

#endif //___APPLE__
