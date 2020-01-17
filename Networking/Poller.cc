//
// Poller.cc
//
// Copyright © 2019 Couchbase. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "Poller.hh"
#include "Error.hh"
#include "Logging.hh"
#include "ThreadUtil.hh"
#include "PlatformIO.hh"
#include "c4Base.h"
#include "sockpp/platform.h"
#include "sockpp/tcp_acceptor.h"
#include "sockpp/tcp_connector.h"
#include <vector>
#include <errno.h>

#ifndef _WIN32
#include <unistd.h>
#include <poll.h>
#endif

#define WSLog (*(LogDomain*)kC4WebSocketLog)
#define LOG(LEVEL, ...) LogToAt(WSLog, LEVEL, ##__VA_ARGS__)

namespace litecore { namespace net {
    using namespace std;
    using namespace sockpp;



    static int throwSocketError() {
#ifdef _WIN32
        error::_throw(error::POSIX, ::WSAGetLastError());
#else
        error::_throwErrno();
#endif
    }


    Poller::Poller() {
        // To allow poll() system calls to be interrupted, we create a pipe and have poll()
        // watch its read end. Then writing to the pipe will cause poll() to return. As a bonus,
        // we can use the data written to the pipe as a message, to let waitForIO know what happened.
#ifndef _WIN32
        int fd[2];
        if (::pipe(fd) < 0)
            throwSocketError();
        _interruptReadFD = fd[0];
        _interruptWriteFD = fd[1];
#else
        // On Windows, pipes aren't available so we have to create a pair of TCP sockets
        // connected through the loopback interface. <https://stackoverflow.com/a/3333565/98077>
        tcp_acceptor acc(inet_address(INADDR_LOOPBACK, 0));
        if (acc.last_error() != 0)
            throwSocketError();
        tcp_connector readSock(acc.address());
        if (readSock.last_error() != 0)
            throwSocketError();
        tcp_socket writeSock = acc.accept();
        if (writeSock.last_error() != 0)
            throwSocketError();
        _interruptReadFD = readSock.release();
        _interruptWriteFD = writeSock.release();
#endif
    }


    Poller::~Poller() {
        if (_interruptReadFD >= 0) {
#ifndef _WIN32
            ::close(_interruptReadFD);
            ::close(_interruptWriteFD);
#else
            ::closesocket(_interruptReadFD);
            ::closesocket(_interruptWriteFD);
#endif
        }
    }


    /*static*/ Poller& Poller::instance() {
        static Poller* sInstance = new Poller(true);
        return *sInstance;
    }


    void Poller::addListener(int fd, Event event, Listener listener) {
        Assert(fd >= 0);
        lock_guard<mutex> lock(_mutex);
        _listeners[fd][event] = listener;
        if (_waiting)
            interrupt(0);
    }


    void Poller::removeListeners(int fd) {
        Assert(fd >= 0);
        lock_guard<mutex> lock(_mutex);
        if (auto i = _listeners.find(fd); i != _listeners.end())
            _listeners.erase(i);
        // no need to interrupt the poll thread
    }


    void Poller::callAndRemoveListener(int fd, Event event) {
        Listener listener;
        {
            lock_guard<mutex> lock(_mutex);
            auto i = _listeners.find(fd);
            if (i == _listeners.end())
                return;
            auto &lref = i->second[event];
            if (!lref)
                return;
            listener = move(lref);
            lref = nullptr;
        }
        // Unlock mutex before calling listener
        listener();
    }


    void Poller::interrupt(int message) {
#ifdef WIN32
        if(::send(_interruptWriteFD, (const char *)&message, sizeof(message), 0) < 0)
#else
        if(::write(_interruptWriteFD, &message, sizeof(message)) < 0)
#endif
            throwSocketError();
    }


    Poller& Poller::start() {
        _thread = thread([=] {
            SetThreadName("CBL Networking");
            while (poll())
                ;
        });
        _thread.detach();
        return *this;
    }


    void Poller::stop() {
        interrupt(-1);
        _thread.join();
    }


    bool Poller::poll() {
        // Create the pollfd vector:
        vector<pollfd> pollfds;
        {
            lock_guard<mutex> lock(_mutex);
            pollfds.resize(_listeners.size() + 1);
            auto dst = pollfds.begin();
            for (auto &src : _listeners) {
                short events = 0;
                if (src.second[kReadable])
                    events |= POLLIN;
                if (src.second[kWriteable])
                    events |= POLLOUT;
                if (events)
                    *dst++ = {src.first, events, 0};
            }
            *dst++ = {_interruptReadFD, POLLIN, 0};
            _waiting = true;
        }

        // Wait in poll():
#ifdef WIN32
        while (WSAPoll(pollfds.data(), pollfds.size(), -1) == SOCKET_ERROR) {
#else
        while (::poll(pollfds.data(), nfds_t(pollfds.size()), -1) < 0) {
            if (errno != EINTR)
                return false;
#endif
        }

        _waiting = false;

        // Find the events and dispatch them:
        bool result = true;
        for (pollfd &entry : pollfds) {
            if (entry.revents) {
                auto fd = entry.fd;
                if (fd == _interruptReadFD) {
                    // This is an interrupt -- read the byte from the pipe:
                    int message;
#ifdef WIN32
                    ::recv(_interruptReadFD, (char *)&message, sizeof(message), 0);
#else
                    ::read(_interruptReadFD, &message, sizeof(message));
#endif
                    LOG(Debug, "Poller: interruption %d", message);
                    if (message < 0) {
                        // Receiving a negative message aborts the loop
                        result = false;
                    } else if (message > 0) {
                        // A positive message is a file descriptor to call:
                        callAndRemoveListener(message, kReadable);
                        callAndRemoveListener(message, kWriteable);
                    }
                } else {
                    LOG(Debug, "Poller: fd %d got event 0x%02x", fd, entry.revents);
                    if (entry.revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL))
                        callAndRemoveListener(fd, kReadable);
                    if (entry.revents & (POLLOUT | POLLERR | POLLHUP | POLLNVAL))
                        callAndRemoveListener(fd, kWriteable);
                    if (entry.revents & POLLNVAL)
                        removeListeners(fd);
                }
            }
        }
        return result;
    }

} }
