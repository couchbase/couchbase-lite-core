//
// SocketFactoryTest.cc
//
// Copyright 2025-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "c4Test.hh"
#include "Certificate.hh"
#include "Error.hh"
#include "LiteCoreTest.hh"
#include "StringUtil.hh"
#include "TCPSocketFactory.hh"
#include "TLSCodec.hh"
#include "TLSContext.hh"
#include <chrono>
#include <condition_variable>
#include <mutex>

#if COUCHBASE_ENTERPRISE
#include "Server.hh"
#endif

using namespace fleece;
using namespace litecore;
using namespace litecore::crypto;
using namespace litecore::net;
using namespace std;


/** Utility base class that lets clients wait for properties to change. */
class Waitable {
public:
    C4Error error {};

    virtual ~Waitable() = default;

    virtual void waitFor(function_ref<bool()> predicate, chrono::seconds timeout = 10s) {
        unique_lock lock(_mutex);
        auto result = _cond.wait_for(lock, timeout, predicate);
        if (!result)
            FAIL("Timed out");
        REQUIRE(error == kC4NoError);
    }

    virtual void waitFor(bool const& flag, chrono::seconds timeout = 10s) {
        this->waitFor([&]{ return flag;}, timeout);
    }

protected:
    void notifying(function_ref<void()> fn) {
        unique_lock lock(_mutex);
        fn();
        _cond.notify_all();
    }

    mutex _mutex;
    condition_variable _cond;
};


/** A mock-like C4Socket that just records what happens. */
class DummyC4Socket final : public RefCounted, public C4Socket, public Waitable {
public:
    DummyC4Socket(C4SocketFactory const& factory, void* nativeHandle, bool isServer_)
    : C4Socket{factory, nativeHandle}
    ,isServer(isServer_) {
        if (nativeHandle && _factory.attached)
            _factory.attached(this);
    }

    bool isServer;

    void open(string const& url, slice options = nullslice) {
        REQUIRE(!isServer);
        Address c4addr(url);
        _factory.open(this, (C4Address*)c4addr, options, _factory.context);
    }

    void send(slice data) {
        _factory.write(this, C4SliceResult(alloc_slice(data)));
    }

    void close() {
        _factory.close(this);
    }

    void waitFor(function_ref<bool()> predicate, chrono::seconds timeout = 10s) override {
        Waitable::waitFor([&]{ return predicate() || didClose;}, timeout);
        REQUIRE(predicate());   // check for unexpected close
    }

    void waitFor(bool const& flag, chrono::seconds timeout = 10s) override {
        Waitable::waitFor(flag, timeout);
    }

    bool gotPeerCertificate(slice data, std::string_view hostname) override {
        unique_lock lock(_mutex);
        certData = data;
        certHostname = hostname;
        _cond.notify_all();
        return true;
    }

    void gotHTTPResponse(int status, slice headers) override {
        REQUIRE(!isServer);
        unique_lock lock(_mutex);
        httpStatus = status;
        responseHeadersFleece = headers;
        _cond.notify_all();
    }

    void opened() override {
        unique_lock lock(_mutex);
        didOpen = true;
        _cond.notify_all();
    }

    void closed(C4Error errorIfAny) override {
        unique_lock lock(_mutex);
        didClose = true;
        if (!(isServer && errorIfAny == C4Error(POSIXDomain, ECONNRESET))) // Ignore client closing socket
           error = errorIfAny;
        _cond.notify_all();
    }

    void closeRequested(int status, slice message) override {
        error::_throw(error::Unimplemented); // should not be called
    }

    void completedWrite(size_t byteCount) override {
        unique_lock lock(_mutex);
        bytesWritten += byteCount;
        _cond.notify_all();
    }

    void received(slice data) override {
        unique_lock lock(_mutex);
        if (data.size > 0)
            dataRead += string(data);
        else
            didReadEOF = true;
        _cond.notify_all();
    }

    bool            didOpen = false;
    bool            didReadEOF = false;
    bool            didClose = false;
    alloc_slice     certData;
    string          certHostname;
    int             httpStatus = -1;
    alloc_slice     responseHeadersFleece;
    size_t          bytesWritten = 0;
    string          dataRead;

protected:
    void socket_retain() override {fleece::retain(this);}
    void socket_release() override {fleece::release(this);}
};


static string fixCRLF(string str) {
    replace(str, "\r\n", "\n");
    return str;
}


static void testSocketFactory(string const& hostname, bool withTLS) {
    Retained tcp = new TCPSocketFactory;
    C4SocketFactory factory = tcp->factory();
    void* nativeHandle = tcp.get();

    string url;
    if (withTLS) {
        Log("-------- WITH TLS --------");
        Retained tlsContext = new TLSContext(TLSContext::Client);
        tie(factory, nativeHandle) = wrapSocketInTLS(factory, nativeHandle, tlsContext);
        url = "https://" + hostname;
    } else {
        url = "http://" + hostname;
    }

    auto socket = new DummyC4Socket(factory, nativeHandle, false);
    Retained<C4Socket> retainSocket = socket;   // awkwardness bc Retained<DummyC4Socket> is broken

    string request = "GET / HTTP/1.0\r\nHost: " + hostname + "\r\nConnection: close\r\n\r\n";

    socket->open(url);
    socket->waitFor(socket->didOpen);
    Log("** Socket opened **");
    if (withTLS) {
        CHECK(socket->certData);
        CHECK(socket->certHostname == hostname);
    }
    socket->send(request);
    socket->waitFor([&]{return socket->bytesWritten >= request.size();});
    Log("** Socket delivered HTTP request **");
    socket->waitFor(socket->didReadEOF);
    Log("** Socket EOF -- received  %zu bytes", socket->dataRead.size());
    // cerr << fixCRLF(socket->dataRead);

    socket->close();
    socket->waitFor(socket->didClose);
    Log("** Socket closed **");

    CHECK(socket->dataRead.starts_with("HTTP/1"));
    CHECK(socket->dataRead.size() >= 500);

    CHECK(socket->refCount() == 1);
    Log("** Releasing socket **");
    retainSocket = nullptr;
    Log("** Releasing factory **");
    CHECK(tcp->refCount() == 1);
    tcp = nullptr;
}


struct SocketFactoryTest {
    SocketFactoryTest() :objectCount(c4_getObjectCount()) { }

    ~SocketFactoryTest() {
        if ( !current_exception() ) {
            // Check for leaks:
            if (c4_getObjectCount() != objectCount ) {
                fprintf(stderr, "Checking for leaked objects...\n");
                if ( !WaitUntil(20s, [&] { return c4_getObjectCount() - objectCount == 0; }) ) {
                    FAIL_CHECK("LiteCore objects were leaked by this test:");
                    fprintf(stderr, "*** LEAKED LITECORE OBJECTS: \n");
                    c4_dumpInstances();
                    fprintf(stderr, "***\n");
                }
            }
        }
    }

private:
    const int objectCount;
};


TEST_CASE_METHOD(SocketFactoryTest, "TCPSocketFactory Client") {
    InitTestLogging(kC4LogDebug);
    c4log_setLevel(c4log_getDomain("WS", true), kC4LogDebug);
    testSocketFactory("example.com", false);
}


TEST_CASE_METHOD(SocketFactoryTest, "TLSSocketFactory Client") {
    InitTestLogging(kC4LogDebug);
    c4log_setLevel(c4log_getDomain("WS", true), kC4LogDebug);
    testSocketFactory("www.couchbase.com", true);
}


#pragma mark - SERVER:

#if COUCHBASE_ENTERPRISE


struct SocketFactoryTestDelegate : public REST::Server::Delegate, public Waitable {
    Retained<TLSContext> _tlsContext;
    bool gotConnection = false;
    bool closedConnection = false;

    explicit SocketFactoryTestDelegate(TLSContext* tls) :_tlsContext(tls) { }

    void handleConnection(std::unique_ptr<ResponderSocket> responder) override {
        Log("** Server received a connection");

        Retained tcp = new TCPSocketFactory(std::move(responder));
        C4SocketFactory factory = tcp->factory();
        void* nativeHandle = tcp.get();
        if (_tlsContext) {
            tie(factory, nativeHandle) = wrapSocketInTLS(factory, nativeHandle, _tlsContext);
        }

        auto socket = new DummyC4Socket(factory, nativeHandle, true);
        Retained<C4Socket> retainSocket = socket;   // awkwardness bc Retained<DummyC4Socket> is broken
        notifying([&]{gotConnection = true;});

        Log("Waiting for socket to open...");
        socket->waitFor(socket->didOpen);
        Log("** Server connection opened");
        socket->waitFor([&]{return !socket->dataRead.empty();});
        string dataRead = socket->dataRead;
        Log("** Server received %zu bytes", dataRead.size());
        cerr << fixCRLF(dataRead);

        string response = "HTTP/1.0 200\r\nContent-Type: text/plain\r\n\r\nBeep boop!\n";
        socket->send(response);
        socket->waitFor([&]{return socket->bytesWritten == response.size();});
        Log("** Server sent response");

        socket->close();
        socket->waitFor([&] { return socket->didClose || socket->didReadEOF; });
        Log("** Server connection closed **");
        notifying([&]{closedConnection = true;});
    }
};


static Ref<crypto::Identity> createServerIdentity() {
    Cert::SubjectParameters subjectParams(DistinguishedName("CN=CppTests, O=Couchbase, C=US"_sl));
    subjectParams.nsCertType = NSCertType(SSL_SERVER);
    Cert::IssuerParameters issuerParams;
    issuerParams.validity_secs = 3600 * 24;
    const auto randomSerial    = randomDigitString<16>();
    issuerParams.serial        = slice(randomSerial);
    Retained<PrivateKey> key   = PrivateKey::generateTemporaryRSA(2048);
    Retained<Cert>       cert = new Cert(subjectParams, issuerParams, key);
    return make_retained<Identity>(cert, key);
}


static void testServerSocketFactory(bool withTLS) {
    Retained<TLSContext> tlsContext;
    if (withTLS) {
        Log("-------- WITH TLS --------");
        tlsContext = make_retained<TLSContext>(TLSContext::Server);
        auto identity = createServerIdentity();
        tlsContext->setIdentity(identity);
    }

    SocketFactoryTestDelegate delegate(tlsContext);
    auto server = make_retained<REST::Server>(delegate);
    server->start(26876);
    Log("** Server started**");
    delegate.waitFor([&]{return delegate.closedConnection;}, 9999s);
    server->stop();
}


TEST_CASE_METHOD(SocketFactoryTest, "TCPSocketFactory Server") {
    InitTestLogging(kC4LogDebug);
    c4log_setLevel(c4log_getDomain("WS", true), kC4LogDebug);
    testServerSocketFactory(false);
}


TEST_CASE_METHOD(SocketFactoryTest, "TLSSocketFactory Server") {
    InitTestLogging(kC4LogDebug);
    c4log_setLevel(c4log_getDomain("WS", true), kC4LogDebug);
    testServerSocketFactory(true);
}

#endif
