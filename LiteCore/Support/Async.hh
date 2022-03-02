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
#include "Result.hh"
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


    namespace {
        struct voidPlaceholder; // Just a hack to avoid `void&` parameters in Async<void>
        template <typename T> struct _ThenType       { using type = T; using realType = T; };
        template <>           struct _ThenType<void> { using type = voidPlaceholder; };
    }

#pragma mark - ASYNCPROVIDER:


    // Maintains the context/state of an async operation and its observer.
    // Abstract base class of AsyncProvider<T>
    class AsyncProviderBase : public RefCounted,
                              public fleece::InstanceCountedIn<AsyncProviderBase>
    {
    public:
        bool ready() const                                  {return _ready;}

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
        std::atomic<bool>             _ready {false};         // True when result is ready
    };


    /** An asynchronously-provided result, seen from the producer's side. */
    template <class T>
    class AsyncProvider final : public AsyncProviderBase {
    public:
        using ResultType = Result<T>;
        using ThenType = typename _ThenType<T>::type;

        /// Creates a new empty AsyncProvider.
        static Retained<AsyncProvider> create()            {return new AsyncProvider;}

        /// Creates a new AsyncProvider that already has a result.
        static Retained<AsyncProvider> createReady(ResultType r)  {
            return new AsyncProvider(std::move(r));
        }

        /// Constructs a new empty AsyncProvider.
        AsyncProvider() = default;

        /// Creates the client-side view of the result.
        Async<T> asyncValue()                               {return Async<T>(this);}

        /// Resolves the value by storing a non-error result and waking any waiting clients.
        template <typename RESULT,
                  typename = std::enable_if_t<std::is_constructible_v<ResultType,RESULT>>>
        void setResult(RESULT &&result) {
            std::unique_lock<std::mutex> lock(_mutex);
            precondition(!_result);
            _result.emplace(std::forward<RESULT>(result));
            _gotResult(lock);
        }

//        /// Equivalent to `setResult` but constructs the T value directly inside the provider.
//        template <class... Args,
//                  class = std::enable_if<std::is_constructible_v<ResultType, Args...>>>
//        void emplaceResult(Args&&... args) {
//            std::unique_lock<std::mutex> lock(_mutex);
//            precondition(!_result);
//            _result.emplace(args...);
//            _gotResult(lock);
//        }

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
                setResult(error::convertCurrentException());
            }
        }

        ResultType& result() & {
            std::unique_lock<std::mutex> _lock(_mutex);
            precondition(_result);
            return *_result;
        }

        const ResultType& result() const & {
            return const_cast<AsyncProvider*>(this)->result();
        }

        ResultType result() && {
            return moveResult();
        }

        ResultType moveResult() {
            std::unique_lock<std::mutex> _lock(_mutex);
            precondition(_result);
            return *std::move(_result);
        }

        bool hasError() const {return result().isError();}

        C4Error error() const {
            if (auto x = result().errorPtr())
                return *x;
            else
                return {};
        }

    private:
        friend class AsyncProviderBase;
        friend class Async<T>;

        explicit AsyncProvider(ResultType result)
        :AsyncProviderBase(true)
        ,_result(std::move(result))
        { }

        template <typename U>
        Async<U> _now(std::function<Async<U>(ThenType&&)> &callback) {
            if (hasError())
                return Async<U>(error());
            try {
                return callback(moveResult().get());
            } catch (const std::exception &x) {
                return Async<U>(C4Error::fromException(x));
            }
        }

        std::optional<ResultType> _result;   // My result
    };


#pragma mark - ASYNC:


    // base class of Async<T>
    class AsyncBase {
    public:
        /// Sets which Actor the callback of a `then` call should run on.
        AsyncBase& on(Actor *actor)                         {_onActor = actor; return *this;}

        /// Returns true once the result is available.
        bool ready() const                                  {return _provider->ready();}

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
        using ResultType = Result<T>;
        using ThenType = typename _ThenType<T>::type;

        /// Returns a new AsyncProvider<T>.
        static Retained<AsyncProvider<T>> makeProvider()    {return AsyncProvider<T>::create();}

        /// Creates an Async value from its provider.
        Async(AsyncProvider<T> *provider)                   :AsyncBase(provider) { }
        Async(Retained<AsyncProvider<T>> &&provider)        :AsyncBase(std::move(provider)) { }

        /// Creates an already-resolved Async with a result (success or error.)
        Async(ResultType t)
        :AsyncBase(AsyncProvider<T>::createReady(std::move(t)))
        { }
        template <typename R,
                  typename = std::enable_if_t<std::is_convertible_v<R,ResultType>>>
        Async(R &&r)                        :Async(ResultType(std::forward<R>(r))) { }


        /// Invokes the callback when the result is ready.
        /// The callback should take a single parameter of type `T`, `T&` or `T&&`.
        /// The callback's return type may be:
        /// - `X` -- the `then` method will return `Async<X>`, which will resolve to the callback's
        ///   return value after the callback returns.
        /// - `Async<X>` -- the `then` method will return `Async<X>`. After the callback
        ///   returns, _and_ its returned async value becomes ready, the returned
        ///   async value will resolve to that value.
        /// - `void` -- the `then` method will return `Async<void>`, because you've handled the
        ///   result value (`T`) but not a potential error, so what's left is just the error.
        ///   You can chain `onError()` to handle the error.
        ///
        /// If the async operation fails, i.e. the result is an error not a `T`, your callback will
        /// not be called. Instead the error is passed on to the `Async` value that was returned by
        /// `then()`.
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
        template <typename LAMBDA,
                  typename RV = std::invoke_result_t<LAMBDA,ThenType>,
                  typename = std::enable_if_t<!std::is_void_v<RV>>>
        [[nodiscard]]
        auto then(LAMBDA &&callback) && {
            // U is the return type of the lambda with any `Async<...>` removed
            using U = unwrap_async<RV>;
            return _then<U>(std::forward<LAMBDA>(callback));
        }


        /// `then()` with a callback that returns nothing (`void`).
        /// The callback is called only if the async operation is successful.
        /// The `then` method itself returns an `Async<void>` which conveys the success/failure of
        /// the operation -- you need to handle that value because you haven't handled the failure
        /// case yet. Typically you'd chain an `onError(...)`.
        ///
        /// Also consider using the `then()` method that takes two parameters for success & failure.
        [[nodiscard]]
        Async<void> then(std::function<void(const ThenType&)> &&callback) {
            auto result = Async<void>::makeProvider();
            _provider->setObserver(_onActor, [=](AsyncProviderBase &providerBase) {
                auto &provider = dynamic_cast<AsyncProvider<T>&>(providerBase);
                if (provider.hasError())
                    result->setResult(provider.error());
                else {
                    callback(provider.moveResult().get());
                    result->setResult(Result<void>());
                }
            });
            return result->asyncValue();
        }


        /// Special `then()` for `Async<void>` only. The callback takes a `C4Error` and returns
        /// nothing. It's called on sucess or failure; on success, the C4Error will have `code` 0.
        void then(std::function<void(C4Error)> &&callback) {
            static_assert(std::is_same_v<T, void>,
                          "then(C4Error) is only allowed with Async<void>; use onError()");
            _provider->setObserver(_onActor, [=](AsyncProviderBase &providerBase) {
                auto &provider = dynamic_cast<AsyncProvider<T>&>(providerBase);
                callback(provider.error());
            });
        }


        /// Version of `then` that takes _two_ callbacks, one for the result and one for an error.
        /// There is a function `assertNoAsyncError` that can be passed as the second parameter if
        /// you are certain there will be no error.
        template <typename LAMBDA>
        void then(LAMBDA &&callback,
                  std::function<void(C4Error)> errorCallback) &&
        {
            std::function<void(T)> fn = std::forward<LAMBDA>(callback);
            _then(std::move(fn), std::move(errorCallback));
        }


        /// Invokes the callback when the result is ready, but only if it's an error.
        /// A successful result is ignored. Normally chained after a `then` call to handle the
        /// remaining error condition.
        void onError(std::function<void(C4Error)> callback) {
            _provider->setObserver(_onActor, [=](AsyncProviderBase &providerBase) {
                auto &provider = dynamic_cast<AsyncProvider<T>&>(providerBase);
                if (provider.hasError())
                    callback(provider.error());
            });
        }


        /// Returns the result. (Throws an exception if the result is not yet available.)
        /// If the result contains an error, throws that as an exception.
        ResultType& result() &                               {return provider()->result();}
        ResultType result() &&                               {return provider()->result();}

        /// Move-returns the result. (Throws an exception if the result is not yet available.)
        /// If the result contains an error, throws that as an exception.
        ResultType moveResult()                             {return provider()->moveResult();}

        /// Blocks the current thread until the result is available, then returns it.
        /// If the result contains an error, throws that as an exception.
        /// \warning  This is intended for use in unit tests. Please don't use it otherwise unless
        ///           absolutely necessary; use `then()` or `AWAIT()` instead.
        ResultType& blockingResult() {
            blockUntilReady();
            return result();
        }

        /// Returns the error result, else nullptr.
        C4Error error() const                             {return provider()->error();}

    private:
        friend class Actor;
        template <class U> friend class Async;

        Async() :AsyncBase(makeProvider()) { }

        AsyncProvider<T>* provider()                {return (AsyncProvider<T>*)_provider.get();}
        AsyncProvider<T> const* provider() const    {return (const AsyncProvider<T>*)_provider.get();}


        void thenProvider(std::function<void(AsyncProvider<T>&)> callback) && {
            _provider->setObserver(_onActor, [=](AsyncProviderBase &provider) {
                callback(dynamic_cast<AsyncProvider<T>&>(provider));
            });
        }


        // Implements `then` where the lambda returns a regular type `U`. Returns `Async<U>`.
        template <typename U>
        [[nodiscard]]
        Async<U> _then(std::function<U(ThenType&&)> &&callback) {
            auto uProvider = Async<U>::makeProvider();
            if (canCallNow()) {
                // Result is available now, so call the callback:
                if (C4Error x = error())
                    uProvider->setResult(x);
                else
                    uProvider->setResultFromCallback([&]{return callback(moveResult());});
            } else {
                _provider->setObserver(_onActor, [=](AsyncProviderBase &providerBase) {
                    auto provider = dynamic_cast<AsyncProvider<T>&>(providerBase);
                    if (auto x = provider.error())
                        uProvider->setError(*x);
                    else {
                        uProvider->setResultFromCallback([&]{
                            return callback(provider.moveResult());
                        });
                    }
                });
            }
            return uProvider->asyncValue();
        }


        // Implements `then` where the lambda returns `Async<U>`.
        template <typename U>
        [[nodiscard]]
        Async<U> _then(std::function<Async<U>(ThenType&&)> &&callback) {
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
                    std::move(asyncU).thenProvider([uProvider](auto &provider) {
                        // Then finally resolve the async I returned:
                        uProvider->setResult(provider.result());
                    });
                });
                return uProvider->asyncValue();
            }
        }


        // innards of the `then()` with two callbacks
        template <typename R,
        typename = std::enable_if_t<std::is_convertible_v<T,R>>>
        void _then(std::function<void(R)> &&callback,
                   std::function<void(C4Error)> &&errorCallback)
        {
            _provider->setObserver(_onActor, [=](AsyncProviderBase &providerBase) {
                auto &provider = dynamic_cast<AsyncProvider<T>&>(providerBase);
                if (provider.hasError())
                    errorCallback(provider.error());
                else
                    callback(provider.moveResult().get());
            });
        }

    };


    /// You can use this as the error-handling callback to `void Async<T>::then(onResult,onError)`.
    /// It throws an assertion-failed exception if the C4Error's code is nonzero.
    void assertNoAsyncError(C4Error);

}
