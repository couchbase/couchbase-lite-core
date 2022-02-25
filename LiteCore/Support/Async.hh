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
#include "RefCounted.hh"
#include "InstanceCounted.hh"
#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <utility>
#include <betterassert.hh>

namespace litecore::actor {
    using fleece::RefCounted;
    using fleece::Retained;
    class Actor;
    class AsyncBase;
    class AsyncProviderBase;
    template <class T> class Async;
    template <class T> class AsyncProvider;


    // *** For full documentation, read Networking/BLIP/docs/Async.md ***


    // Interface for observing when an Async value becomes available.
    class AsyncObserver {
    public:
        virtual ~AsyncObserver() = default;
        void notifyAsyncResultAvailable(AsyncProviderBase*, Actor*);
    protected:
        virtual void asyncResultAvailable(Retained<AsyncProviderBase>) =0;
    };


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

        /// Returns the exception result, else nullptr.
        std::exception_ptr exception() const                {return _exception;}

        /// Sets an exception as the result. This will wake up observers.
        void setException(std::exception_ptr);

        /// If the result is an exception, re-throws it. Else does nothing.
        void rethrowException() const;

        void setObserver(AsyncObserver*, Actor* =nullptr);

    protected:
        friend class AsyncBase;

        explicit AsyncProviderBase(bool ready = false);
        explicit AsyncProviderBase(Actor *actorOwningFn, const char *functionName);
        ~AsyncProviderBase();
        void _gotResult(std::unique_lock<std::mutex>&);

        std::mutex mutable            _mutex;
    private:
        AsyncObserver*                _observer {nullptr};    // AsyncObserver waiting on me
        Retained<Actor>               _observerActor;         // Actor the observer was running on
        std::exception_ptr            _exception {nullptr};   // Exception if provider failed
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
                setException(std::current_exception());
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
            rethrowException();
            precondition(_result);
            return *_result;
        }

        T result() && {
            return extractResult();
        }

        T extractResult() {
            std::unique_lock<std::mutex> _lock(_mutex);
            rethrowException();
            precondition(_result);
            return *std::move(_result);
        }

        template <typename U>
        Async<U> _now(std::function<Async<U>(T&&)> &callback) {
            if (auto x = exception())
                return Async<U>(x);
            try {
                return callback(extractResult());
            } catch (...) {
                return Async<U>(std::current_exception());
            }
        }

        std::optional<T> _result;   // My result
    };


#pragma mark - ASYNC:


    /// Compile-time utility that pulls the result type out of an Async type.
    /// If `T` is `Async<X>`, or a reference thereto, then `async_result_type<T>` is X.
    template <class T>
    using async_result_type = typename std::remove_reference_t<T>::ResultType;

    namespace {
        // Magic template gunk. `unwrap_async<T>` removes a layer of `Async<...>` from a type:
        // - `unwrap_async<string>` is `string`.
        // - `unwrap_async<Async<string>> is `string`.
        template <typename T> T _unwrap_async(T*);
        template <typename T> T _unwrap_async(Async<T>*);
        template <typename T> using unwrap_async = decltype(_unwrap_async((T*)nullptr));
    }


    // base class of Async<T>
    class AsyncBase {
    public:
        /// Sets which Actor the callback of a `then` call should run on.
        AsyncBase& on(Actor *actor)                         {_onActor = actor; return *this;}

        /// Returns true once the result is available.
        bool ready() const                                  {return _provider->ready();}

        /// Returns the exception result, else nullptr.
        std::exception_ptr exception() const                {return _provider->exception();}

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

        /// Creates an already-resolved Async with an exception.
        explicit Async(std::exception_ptr x)
        :AsyncBase(makeProvider())
        {
            _provider->setException(x);
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
        auto then(LAMBDA &&callback) && {
            using U = unwrap_async<std::invoke_result_t<LAMBDA,T&&>>; // return type w/o Async<>
            return _then<U>(std::forward<LAMBDA>(callback));
        }

        /// Returns the result. (Throws an exception if the result is not yet available.)
        /// If the result contains an exception, throws that exception.
        T& result() &                               {return _provider->result<T>();}
        T result() &&                               {return _provider->result<T>();}

        /// Move-returns the result. (Throws an exception if the result is not yet available.)
        /// If the result contains an exception, throws that exception.
        T extractResult()                           {return _provider->extractResult<T>();}

        /// Blocks the current thread until the result is available, then returns it.
        /// If the result contains an exception, throws that exception.
        /// \warning  This is intended for use in unit tests. Please don't use it otherwise unless
        ///           absolutely necessary; use `then()` or `AWAIT()` instead.
        T& blockingResult() {
            blockUntilReady();
            return result();
        }

        using ResultType = T;
        using ThenReturnType = Async<T>;

    private:
        class Waiter; // defined below

        AsyncProvider<T>* provider() {
            return (AsyncProvider<T>*)_provider.get();
        }

        // Implements `then` where the lambda returns a regular type `U`. Returns `Async<U>`.
        template <typename U>
        typename Async<U>::ThenReturnType
        _then(std::function<U(T&&)> &&callback) {
            auto uProvider = Async<U>::makeProvider();
            if (canCallNow()) {
                // Result is available now, so call the callback:
                if (auto x = exception())
                    uProvider->setException(x);
                else
                    uProvider->setResultFromCallback([&]{return callback(extractResult());});
            } else {
                // Create an AsyncWaiter to wait on the provider:
                Waiter::start(this->provider(), _onActor, [uProvider,callback](auto &provider) {
                    if (auto x = provider.exception())
                        uProvider->setException(x);
                    else {
                        uProvider->setResultFromCallback([&]{return callback(provider.extractResult());});
                    }
                });
            }
            return uProvider->asyncValue();
        }

        // Implements `then` where the lambda returns void. (Specialization of above method.)
        template<>
        void _then<void>(std::function<void(T&&)> &&callback) {
            if (canCallNow())
                callback(extractResult());
            else
                Waiter::start(provider(), _onActor, [=](auto &provider) {
                    callback(provider.extractResult());
                });
        }

        // Implements `then` where the lambda returns `Async<U>`.
        template <typename U>
        Async<U> _then(std::function<Async<U>(T&&)> &&callback) {
            if (canCallNow()) {
                // If I'm ready, just call the callback and pass on the Async<U> it returns:
                return provider()->_now(callback);
            } else {
                // Otherwise wait for my result...
                auto uProvider = Async<U>::makeProvider();
                Waiter::start(provider(), _onActor, [=] (auto &provider) mutable {
                    // Invoke the callback, then wait to resolve the Async<U> it returns:
                    auto asyncU = provider._now(callback);
                    std::move(asyncU).then([uProvider](U &&uresult) {
                        // Then finally resolve the async I returned:
                        uProvider->setResult(std::forward<U>(uresult));
                    });
                });
                return uProvider->asyncValue();
            }
        }

    };


    //---- Implementation gunk...


#ifndef _THISACTOR_DEFINED
#define _THISACTOR_DEFINED
    // Used by `BEGIN_ASYNC` macros. Returns the lexically enclosing actor instance, else NULL.
    // (How? Outside of an Actor method, `thisActor()` refers to the function below.
    // In an Actor method, it refers to `Actor::thisActor()`, which returns `this`.)
    static inline Actor* thisActor() {return nullptr;}
#endif


    template <typename T>
    T& AsyncProviderBase::result() {
        return dynamic_cast<AsyncProvider<T>*>(this)->result();
    }

    template <typename T>
    T AsyncProviderBase::extractResult() {
        return dynamic_cast<AsyncProvider<T>*>(this)->extractResult();
    }


    // Internal class used by `Async<T>::then()`, above
    template <typename T>
    class Async<T>::Waiter : public AsyncObserver {
    public:
        using Callback = std::function<void(AsyncProvider<T>&)>;

        static void start(AsyncProvider<T> *provider, Actor *onActor, Callback &&callback) {
            (void) new Waiter(provider, onActor, std::forward<Callback>(callback));
        }

    protected:
        Waiter(AsyncProvider<T> *provider, Actor *onActor, Callback &&callback)
        :_callback(std::move(callback))
        {
            provider->setObserver(this, onActor);
        }

        void asyncResultAvailable(Retained<AsyncProviderBase> ctx) override {
            auto provider = dynamic_cast<AsyncProvider<T>*>(ctx.get());
            _callback(*provider);
            delete this;            // delete myself when done!
        }
    private:
        Callback _callback;
    };


    // Specialization of AsyncProvider for use in functions with no return value (void).
    // Not used directly, but it's used as part of the implementation of void-returning async fns.
    template <>
    class AsyncProvider<void> : public AsyncProviderBase {
    public:
//        static Retained<AsyncProvider> create()             {return new AsyncProvider;}
//        AsyncProvider() = default;

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
