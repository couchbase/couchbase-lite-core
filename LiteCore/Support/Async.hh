//
// Async.hh
//
// Copyright 2018-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "AsyncActorCommon.hh"
#include "Error.hh"
#include "RefCounted.hh"
#include "InstanceCounted.hh"
#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <utility>
#include <betterassert.hh>

struct C4Error;

namespace litecore::actor {
    using fleece::RefCounted;
    using fleece::Retained;
    class Actor;
    class AsyncBase;
    class AsyncProviderBase;
    template <class T> class Async;
    template <class T> class AsyncProvider;


    // *** For full documentation, read Networking/BLIP/docs/Async.md ***


#pragma mark - ASYNCPROVIDER:


    // Maintains the context/state of an async operation and its observer.
    // Abstract base class of AsyncProvider<T>
    class AsyncProviderBase : public RefCounted,
                              public fleece::InstanceCountedIn<AsyncProviderBase>
    {
    public:
        bool ready() const                                  {return _ready;}

        template <typename T> T& result();
        template <typename T> T extractResult();

        /// Returns the error/exception result, else nullptr.
        litecore::error* error() const                      {return _error.get();}
        C4Error c4Error() const;

        /// Sets an error as the result. This will wake up observers.
        void setError(const std::exception&);
        void setError(const C4Error&);

        /// If the result is an error, throws it as an exception. Else does nothing.
        void throwIfError() const;

        using Observer = std::function<void(AsyncProviderBase&)>;

        void setObserver(Actor*, Observer);

    protected:
        friend class AsyncBase;

        explicit AsyncProviderBase(bool ready = false);
        explicit AsyncProviderBase(Actor *actorOwningFn, const char *functionName);
        ~AsyncProviderBase();
        void _gotResult(std::unique_lock<std::mutex>&);
        void notifyObserver(Observer &observer, Actor *actor);

        std::mutex mutable            _mutex;
    private:
        Observer                      _observer;
        Retained<Actor>               _observerActor;         // Actor the observer was running on
        std::unique_ptr<litecore::error> _error;             // Error if provider failed
        std::atomic<bool>             _ready {false};         // True when result is ready
    };


    /** An asynchronously-provided result, seen from the producer's side. */
    template <class T>
    class AsyncProvider final : public AsyncProviderBase {
    public:
        using ResultType = T;

        /// Creates a new empty AsyncProvider.
        static Retained<AsyncProvider> create()            {return new AsyncProvider;}

        /// Creates a new AsyncProvider that already has a result.
        static Retained<AsyncProvider> createReady(T&& r)  {
            return new AsyncProvider(std::forward<T>(r));
        }

        /// Constructs a new empty AsyncProvider.
        AsyncProvider() = default;

        /// Creates the client-side view of the result.
        Async<T> asyncValue()                               {return Async<T>(this);}

        /// Resolves the value by storing the result and waking any waiting clients.
        void setResult(const T &result) {
            std::unique_lock<std::mutex> lock(_mutex);
            precondition(!_result);
            _result = result;
            _gotResult(lock);
        }

        /// Resolves the value by move-storing the result and waking any waiting clients.
        void setResult(T &&result) {
            std::unique_lock<std::mutex> lock(_mutex);
            precondition(!_result);
            _result = std::forward<T>(result);
            _gotResult(lock);
        }

        /// Equivalent to `setResult` but constructs the T value directly inside the provider.
        template <class... Args,
                  class = std::enable_if<std::is_constructible_v<ResultType, Args...>>>
        void emplaceResult(Args&&... args) {
            std::unique_lock<std::mutex> lock(_mutex);
            precondition(!_result);
            _result.emplace(args...);
            _gotResult(lock);
        }

        template <typename LAMBDA>
        void setResultFromCallback(LAMBDA &&callback) {
            bool duringCallback = true;
            try {
                auto result = callback();
                duringCallback = false;
                setResult(std::move(result));
            } catch (...) {
                if (!duringCallback)
                    throw;
                setError(std::current_exception());
            }
        }

    private:
        friend class AsyncProviderBase;
        friend class Async<T>;

        explicit AsyncProvider(T&& result)
        :AsyncProviderBase(true)
        ,_result(std::move(result))
        { }

        T& result() & {
            std::unique_lock<std::mutex> _lock(_mutex);
            throwIfError();
            precondition(_result);
            return *_result;
        }

        T result() && {
            return extractResult();
        }

        T extractResult() {
            std::unique_lock<std::mutex> _lock(_mutex);
            throwIfError();
            precondition(_result);
            return *std::move(_result);
        }

        template <typename U>
        Async<U> _now(std::function<Async<U>(T&&)> &callback) {
            if (auto x = error())
                return Async<U>(*x);
            try {
                return callback(extractResult());
            } catch (const std::exception &x) {
                return Async<U>(error::convertException(x));
            }
        }

        std::optional<T> _result;   // My result
    };


#pragma mark - ASYNC:


    // base class of Async<T>
    class AsyncBase {
    public:
        /// Sets which Actor the callback of a `then` call should run on.
        AsyncBase& on(Actor *actor)                         {_onActor = actor; return *this;}

        /// Returns true once the result is available.
        bool ready() const                                  {return _provider->ready();}

        /// Returns the error result, else nullptr.
        litecore::error* error() const                      {return _provider->error();}

        C4Error c4Error() const;

        /// Blocks the current thread (i.e. doesn't return) until the result is available.
        /// \warning  This is intended for use in unit tests. Please don't use it otherwise unless
        ///           absolutely necessary; use `then()` or `AWAIT()` instead.
        void blockUntilReady();

    protected:
        explicit AsyncBase(Retained<AsyncProviderBase> &&provider) :_provider(std::move(provider)) { }
        bool canCallNow() const;

        Retained<AsyncProviderBase> _provider;          // The provider that owns my value
        Actor*                      _onActor {nullptr}; // Actor that `then` should call on
    };


    /** An asynchronously-provided result, seen from the consumer's side. */
    template <class T>
    class Async : public AsyncBase {
    public:
        /// Returns a new AsyncProvider<T>.
        static Retained<AsyncProvider<T>> makeProvider()    {return AsyncProvider<T>::create();}

        /// Creates an Async value from its provider.
        Async(AsyncProvider<T> *provider)                   :AsyncBase(provider) { }
        Async(Retained<AsyncProvider<T>> &&provider)        :AsyncBase(std::move(provider)) { }

        /// Creates an already-resolved Async with a value.
        Async(T&& t)
        :AsyncBase(AsyncProvider<T>::createReady(std::forward<T>(t)))
        { }

        /// Creates an already-resolved Async with an error.
        Async(const litecore::error &x)
        :AsyncBase(makeProvider())
        {
            _provider->setError(x);
        }

        Async(const C4Error &err)
        :AsyncBase(makeProvider())
        {
            _provider->setError(err);
        }

        /// Invokes the callback when the result is ready.
        /// The callback should take a single parameter of type `T`, `T&` or `T&&`.
        /// The callback's return type may be:
        /// - `void` -- the `then` method will return `void`.
        /// - `X` -- the `then` method will return `Async<X>`, which will resolve to the callback's
        ///   return value after the callback returns.
        /// - `Async<X>` -- the `then` method will return `Async<X>`. After the callback
        ///   returns, _and_ its returned async value becomes ready, the returned
        ///   async value will resolve to that value.
        ///
        /// By default the callback will be invoked either on the thread that set the result,
        /// or if the result is already available, synchronously on the current thread
        /// (before `then` returns.)
        ///
        /// If an Actor method is calling `then`, it should first call `on(this)` to specify that
        /// it wants the callback to run on its event queue; e.g. `a.on(this).then(...)`.
        ///
        /// Examples:
        /// - `a.then([](T) -> void { ... });`
        /// - `Async<X> x = a.then([](T) -> X { ... });`
        /// - `Async<X> x = a.then([](T) -> Async<X> { ... });`
        template <typename LAMBDA>
        [[nodiscard]]
        auto then(LAMBDA &&callback) && {
            using U = unwrap_async<std::invoke_result_t<LAMBDA,T&&>>; // return type w/o Async<>
            return _then<U>(std::forward<LAMBDA>(callback));
        }

        void then(std::function<void(T)> callback, std::function<void(C4Error)> errorCallback) && {
            _provider->setObserver(_onActor, [=](AsyncProviderBase &provider) {
                if (provider.error())
                    errorCallback(provider.c4Error());
                else
                    callback(provider.extractResult<T>());
            });
        }


        /// Returns the result. (Throws an exception if the result is not yet available.)
        /// If the result contains an error, throws that as an exception.
        T& result() &                               {return _provider->result<T>();}
        T result() &&                               {return _provider->result<T>();}

        /// Move-returns the result. (Throws an exception if the result is not yet available.)
        /// If the result contains an error, throws that as an exception.
        T extractResult()                           {return _provider->extractResult<T>();}

        /// Blocks the current thread until the result is available, then returns it.
        /// If the result contains an error, throws that as an exception.
        /// \warning  This is intended for use in unit tests. Please don't use it otherwise unless
        ///           absolutely necessary; use `then()` or `AWAIT()` instead.
        T& blockingResult() {
            blockUntilReady();
            return result();
        }

        using ResultType = T;
        using ThenReturnType = Async<T>;

    private:
        AsyncProvider<T>* provider() {
            return (AsyncProvider<T>*)_provider.get();
        }

        // Implements `then` where the lambda returns a regular type `U`. Returns `Async<U>`.
        template <typename U>
        [[nodiscard]]
        typename Async<U>::ThenReturnType
        _then(std::function<U(T&&)> &&callback) {
            auto uProvider = Async<U>::makeProvider();
            if (canCallNow()) {
                // Result is available now, so call the callback:
                if (auto x = error())
                    uProvider->setError(x);
                else
                    uProvider->setResultFromCallback([&]{return callback(extractResult());});
            } else {
                _provider->setObserver(_onActor, [=](AsyncProviderBase &provider) {
                    if (auto x = provider.error())
                        uProvider->setError(x);
                    else {
                        uProvider->setResultFromCallback([&]{
                            return callback(provider.extractResult<U>());
                        });
                    }
                });
            }
            return uProvider->asyncValue();
        }

        // Implements `then` where the lambda returns void. (Specialization of above method.)
        template<>
        void _then<void>(std::function<void(T&&)> &&callback) {
            if (canCallNow()) {
                callback(extractResult());
            } else {
                _provider->setObserver(_onActor, [=](AsyncProviderBase &provider) {
                    callback(provider.extractResult<T>());
                });
            }
        }

        // Implements `then` where the lambda returns `Async<U>`.
        template <typename U>
        [[nodiscard]]
        Async<U> _then(std::function<Async<U>(T&&)> &&callback) {
            if (canCallNow()) {
                // If I'm ready, just call the callback and pass on the Async<U> it returns:
                return provider()->_now(callback);
            } else {
                // Otherwise wait for my result...
                auto uProvider = Async<U>::makeProvider();
                _provider->setObserver(_onActor, [=](AsyncProviderBase &provider) mutable {
                    // Invoke the callback, then wait to resolve the Async<U> it returns:
                    auto &tProvider = dynamic_cast<AsyncProvider<T>&>(provider);
                    auto asyncU = tProvider._now(callback);
                    std::move(asyncU).then([uProvider](U &&uresult) {
                        // Then finally resolve the async I returned:
                        uProvider->setResult(std::forward<U>(uresult));
                    }, [uProvider](C4Error err) {
                        uProvider->setError(err);
                    });
                });
                return uProvider->asyncValue();
            }
        }

    };


    //---- Implementation gunk...


    template <typename T>
    T& AsyncProviderBase::result() {
        return dynamic_cast<AsyncProvider<T>*>(this)->result();
    }

    template <typename T>
    T AsyncProviderBase::extractResult() {
        return dynamic_cast<AsyncProvider<T>*>(this)->extractResult();
    }


    // Specialization of AsyncProvider for use in functions with no return value (void).
    template <>
    class AsyncProvider<void> : public AsyncProviderBase {
    private:
        friend class Async<void>;

        void setResult() {
            std::unique_lock<std::mutex> lock(_mutex);
            _gotResult(lock);
        }
    };


    // Specialization of Async<> for `void` type; not used directly.
    template <>
    class Async<void> : public AsyncBase {
    public:
        using ThenReturnType = void;

        Async(AsyncProvider<void> *provider)                 :AsyncBase(provider) { }
        Async(const Retained<AsyncProvider<void>> &provider) :AsyncBase(provider) { }
    };

}
