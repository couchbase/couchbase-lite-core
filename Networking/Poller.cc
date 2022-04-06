//
// Poller.cc
//
// Copyright 2019-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
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
        _listeners[fd][event] = move(listener);
        if (_waiting)
            _interrupt(0);  // wake the poller thread so it will detect the new listener fd
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


    void Poller::_interrupt(int message) {
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
        _interrupt(-1);
        _thread.join();
    }


    void Poller::interrupt(int fd) {
        Assert(fd > 0);
        _interrupt(fd);
    }

    
#pragma mark - BACKGROUND THREAD:

    
#ifdef WIN32
    // WSAPoll has proven to be weirdly unreliable, so fall back
    // to a select based implementation
    bool Poller::poll() {
        fd_set fds_read, fds_write, fds_err;
        SOCKET maxfd = -1;
        vector<SOCKET> all_fds;
        {
            FD_ZERO(&fds_err);
            FD_ZERO(&fds_read);
            FD_ZERO(&fds_write);

            lock_guard<mutex> lock(_mutex);
            for (auto &src : _listeners) {
                bool included = false;
                if (src.second[kReadable]) {
                    FD_SET(src.first, &fds_read);
                    FD_SET(src.first, &fds_err);
                    if(src.first > maxfd) {
                        maxfd = src.first;
                    }

                    all_fds.push_back(src.first);
                    included = true;
                }

                if (src.second[kWriteable]) {
                    FD_SET(src.first, &fds_write);
                    if(!included) {
                        FD_SET(src.first, &fds_err);
                        if(src.first > maxfd) {
                            maxfd = src.first;
                        }

                        all_fds.push_back(src.first);
                    }
                }
            }

            FD_SET(_interruptReadFD, &fds_read);
            FD_SET(_interruptReadFD, &fds_err);
            if(_interruptReadFD > maxfd) {
                maxfd = _interruptReadFD;
            }
            _waiting = true;
        }

        while(select(maxfd, &fds_read, &fds_write, &fds_err, nullptr) == SOCKET_ERROR) {
            if(WSAGetLastError() != WSAEINTR) {
                LogError(WSLog, "Poller: poll() returned WSA error %d; stopping thread", WSAGetLastError());
                return false;
            }
        }

        _waiting = false;
        bool result = true;
        if(FD_ISSET(_interruptReadFD, &fds_read)) {
            int message;
            ::recv(_interruptReadFD, (char *)&message, sizeof(message), 0);
            LogDebug(WSLog, "Poller: interruption %d", message);
            if (message < 0) {
                // Receiving a negative message aborts the loop
                LogTo(WSLog, "Poller: thread is stopping");
                result = false;
            } else if (message > 0) {
                // A positive message is a file descriptor to call:
                LogDebug(WSLog, "Poller: fd %d is disconnected", message);
                callAndRemoveListener(message, kDisconnected);
                removeListeners(message);
            }
        }

        for (SOCKET s : all_fds) {
            if(FD_ISSET(s, &fds_read)) {
                LogDebug(WSLog, "Poller: socket %d got read event", s);
                 callAndRemoveListener(s, kReadable);
            }

            if(FD_ISSET(s, &fds_write)) {
                LogDebug(WSLog, "Poller: socket %d got write event", s);
                callAndRemoveListener(s, kWriteable);
            }

            if(FD_ISSET(s, &fds_err)) {
                LogDebug(WSLog, "Poller: socket %d got error", s);
                callAndRemoveListener(s, kDisconnected);
                removeListeners(s);
            }
        }

        return result;
    }

#else

    bool Poller::poll() {
        // Create the pollfd vector:
        vector<pollfd> pollfds;
        {
            lock_guard<mutex> lock(_mutex);
            for (auto &src : _listeners) {
                short events = 0;
                if (src.second[kReadable])
                    events |= POLLIN;
                if (src.second[kWriteable])
                    events |= POLLOUT;
                if (events)
                    pollfds.push_back({src.first, events, 0});
            }
            pollfds.push_back({_interruptReadFD, POLLIN, 0});
            _waiting = true;
        }

        // Wait in poll():
        while (::poll(pollfds.data(), nfds_t(pollfds.size()), -1) < 0) {
            if (errno != EINTR) {
                LogError(WSLog, "Poller: poll() returned errno %d; stopping thread", errno);
                return false;
            }
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
                    auto nread = ::read(_interruptReadFD, &message, sizeof(message));
                    if(_usuallyFalse(nread == -1)) {
                        result = false;
                    } else if (message < 0) {
                        // Receiving a negative message aborts the loop
                        LogTo(WSLog, "Poller: thread is stopping");
                        result = false;
                    } else if (message > 0) {
                        // A positive message is a file descriptor to tell it's disconnected:
                        fd = message;
                        LogDebug(WSLog, "Poller: fd %d is disconnected", fd);
                        callAndRemoveListener(fd, kDisconnected);
                        removeListeners(fd);
                    }
                } else {
                    LogDebug(WSLog, "Poller: fd %d got event 0x%02x", fd, entry.revents);
                    if (entry.revents & (POLLIN | POLLHUP))
                        callAndRemoveListener(fd, kReadable);
                    if (entry.revents & POLLOUT)
                        callAndRemoveListener(fd, kWriteable);
                    if (entry.revents & (POLLNVAL | POLLERR)) {
                        callAndRemoveListener(fd, kDisconnected);
                        removeListeners(fd);
                    }
                }
            }
        }
        return result;
    }

#endif

} }
