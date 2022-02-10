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

// *** For full documentation, read Networking/BLIP/docs/Async.md ***


#pragma mark - ASYNC/AWAIT MACROS:


/// Put this at the top of an async function/method that returns `Async<T>`,
/// but below declarations of any variables that need to be in scope for the whole method.
#define BEGIN_ASYNC_RETURNING(T) \
    return Async<T>(_thisActor(), [=](AsyncFnState &_async_state_) mutable -> std::optional<T> { \
        switch (_async_state_.currentLine()) { \
            default:

/// Put this at the top of an async method that returns `void`.
/// See `BEGIN_ASYNC_RETURNING` for details.
#define BEGIN_ASYNC() \
    return AsyncFnState::asyncVoidFn(_thisActor(), [=](AsyncFnState &_async_state_) mutable -> void { \
        switch (_async_state_.currentLine()) { \
            default:

/// Use this in an async method to resolve an `Async<>` value, blocking until it's available.
/// `VAR` is the name of the variable to which to assign the result.
/// `EXPR` is the expression (usually a call to an async method) returning an `Async<T>` value,
///        where `T` can be assigned to `VAR`.
/// If the `Async` value's result is already available, it is immediately assigned to `VAR` and
/// execution continues.
/// Otherwise, this method is suspended until the result becomes available.
#define AWAIT(VAR, EXPR) \
                if (_async_state_._await(EXPR, __LINE__)) return {}; \
            case __LINE__: \
                VAR = _async_state_.awaited<async_result_type<decltype(EXPR)>>()->extractResult();

/// Put this at the very end of an async function/method.
#define END_ASYNC() \
        } \
    });


    class AsyncBase;
    class AsyncProviderBase;
    template <class T> class Async;
    template <class T> class AsyncProvider;


    // The state data passed to the lambda of an async function. Internal use only.
    class AsyncFnState {
    public:
        int currentLine() const                             {return _currentLine;}
        bool _await(const AsyncBase &a, int curLine);
        template <class T> Retained<AsyncProvider<T>> awaited();

        static void asyncVoidFn(Actor *actor, std::function<void(AsyncFnState&)> body);

   protected:
        friend class AsyncProviderBase;
        template <typename T> friend class AsyncProvider;

        AsyncFnState(AsyncProviderBase *owningProvider, Actor *owningActor)
        :_owningProvider(owningProvider)
        ,_owningActor(owningActor)
        { }

        void updateFrom(AsyncFnState&);

        Retained<AsyncProviderBase> _owningProvider;  // Provider that I belong to
        Retained<Actor>             _owningActor;     // Actor (if any) that owns the async method
        Retained<AsyncProviderBase> _awaiting;        // Provider my fn body is suspended awaiting
        int                         _currentLine {0}; // label/line# to continue body function at
    };


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
                              protected AsyncObserver,
                              public fleece::InstanceCountedIn<AsyncProviderBase>
    {
    public:
        bool ready() const                                  {return _ready;}

        template <typename T> const T& result();
        template <typename T> T&& extractResult();

        /// Returns the exception result, else nullptr.
        std::exception_ptr exception() const                {return _exception;}

        /// Sets an exception as the result. This will wake up observers.
        void setException(std::exception_ptr);

        /// If the result is an exception, re-throws it. Else does nothing.
        void rethrowException() const;

        void setObserver(AsyncObserver*);

    protected:
        friend class AsyncBase;

        explicit AsyncProviderBase(bool ready = false)      :_ready(ready) { }
        explicit AsyncProviderBase(Actor *actorOwningFn);
        ~AsyncProviderBase();
       void _start();
        void _wait();
        void _gotResult(std::unique_lock<std::mutex>&);

        std::mutex mutable            _mutex;
        std::atomic<bool>             _ready {false};         // True when result is ready
        std::exception_ptr            _exception {nullptr};   // Exception if provider failed
        std::unique_ptr<AsyncFnState> _fnState;               // State of associated async fn
    private:
        struct Observer {
            AsyncObserver*     observer = nullptr;    // AsyncObserver waiting on me
            Retained<Actor>    observerActor;         // Actor the observer was running on
        };
        Observer _observer;
    };


    /** An asynchronously-provided result, seen from the producer's side. */
    template <class T>
    class AsyncProvider final : public AsyncProviderBase {
    public:
        using ResultType = T;

        /// Creates a new empty AsyncProvider.
        static Retained<AsyncProvider> create()            {return new AsyncProvider;}

        /// Creates a new AsyncProvider that already has a result.
        static Retained<AsyncProvider> createReady(T&& r)  {return new AsyncProvider(std::move(r));}

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
            _result = std::move(result);
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
        void setResultFromCallback(LAMBDA callback) {
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

        /// Returns the result, which must be available.
        const T& result() const & {
            std::unique_lock<std::mutex> _lock(_mutex);
            rethrowException();
            precondition(_result);
            return *_result;
        }
        
        T&& result() && {
            return extractResult();
        }

        /// Moves the result to the caller. Result must be available.
        T&& extractResult() {
            std::unique_lock<std::mutex> _lock(_mutex);
            rethrowException();
            precondition(_result);
            return *std::move(_result);
        }

    private:
        friend class Async<T>;

        using Body = std::function<std::optional<T>(AsyncFnState&)>;

        explicit AsyncProvider(T&& result)
        :AsyncProviderBase(true)
        ,_result(std::move(result))
        { }

        AsyncProvider(Actor *actor, Body &&body)
        :AsyncProviderBase(actor)
        ,_fnBody(std::move(body))
        { }

        void asyncResultAvailable(Retained<AsyncProviderBase> async) override {
            assert(async == _fnState->_awaiting);
            std::optional<T> r;
            try {
                r = _fnBody(*_fnState);
            } catch(const std::exception &x) {
                setException(std::current_exception());
                return;
            }
            if (r)
                setResult(*std::move(r));
            else
                _wait();
        }

        Body             _fnBody;   // The async function body, if any
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
        /// Returns true once the result is available.
        bool ready() const                                  {return _provider->ready();}

        /// Blocks the current thread (i.e. doesn't return) until the result is available.
        /// Please don't use this unless absolutely necessary; use `then()` or `AWAIT()` instead.
        void blockUntilReady();

        /// Returns the exception result, else nullptr.
        std::exception_ptr exception() const                {return _provider->exception();}

    protected:
        friend class AsyncFnState;
        explicit AsyncBase(Retained<AsyncProviderBase> &&context) :_provider(std::move(context)) { }
        explicit AsyncBase(AsyncProviderBase *context, bool);    // calls context->_start()

        Retained<AsyncProviderBase> _provider;                   // The provider that owns my value
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
        explicit Async(T&& t)
        :AsyncBase(AsyncProvider<T>::createReady(std::move(t)))
        { }

        // (used by `BEGIN_ASYNC_RETURNING(T)`. Don't call directly.)
        Async(Actor *actor, typename AsyncProvider<T>::Body bodyFn)
        :AsyncBase(new AsyncProvider<T>(actor, std::move(bodyFn)), true)
        { }

        /// Returns the result. (Will abort if the result is not yet available.)
        const T& result() const &                           {return _provider->result<T>();}
        T&& result() const &&                               {return _provider->result<T>();}

        /// Move-returns the result. (Will abort if the result is not yet available.)
        T&& extractResult() const                           {return _provider->extractResult<T>();}

        /// Invokes the callback when the result becomes ready (immediately if it's already ready.)
        /// The callback should take a single parameter of type `T`, `T&` or `T&&`.
        /// The callback's return type may be:
        /// - `void` -- the `then` method will return `void`.
        /// - `X` -- the `then` method will return `Async<X>`, which will resolve to the callback's
        ///   return value after the callback returns.
        /// - `Async<X>` -- the `then` method will return `Async<X>`. After the callback
        ///   returns, _and_ its returned async value becomes ready, the returned
        ///   async value will resolve to that value.
        ///
        /// Examples:
        /// - `a.then([](T) -> void { ... });`
        /// - `Async<X> x = a.then([](T) -> X { ... });`
        /// - `Async<X> x = a.then([](T) -> Async<X> { ... });`
        template <typename LAMBDA>
        auto then(LAMBDA callback) {
            using U = unwrap_async<std::result_of_t<LAMBDA(T&&)>>; // return type w/o Async<>
            return _then<U>(callback);
        }

        /// Blocks the current thread until the result is available, then returns it.
        /// Please don't use this unless absolutely necessary; use `then()` or `AWAIT()` instead.
        const T& blockingResult() {
            blockUntilReady();
            return result();
        }

        using ResultType = T;
        using AwaitReturnType = Async<T>;

    private:
        class Waiter; // defined below

        AsyncProvider<T>* provider() {
            return (AsyncProvider<T>*)_provider.get();
        }

        // Implements `then` where the lambda returns a regular type `U`. Returns `Async<U>`.
        template <typename U>
        typename Async<U>::AwaitReturnType _then(std::function<U(T&&)> callback) {
            auto uProvider = Async<U>::makeProvider();
            if (ready()) {
                // Result is available now, so call the callback:
                if (auto x = exception())
                    uProvider->setException(x);
                else
                    uProvider->setResultFromCallback([&]{return callback(extractResult());});
            } else {
                // Create an AsyncWaiter to wait on the provider:
                Waiter::start(this->provider(), [uProvider,callback](T&& result) {
                    uProvider->setResultFromCallback([&]{return callback(std::move(result));});
                });
            }
            return uProvider->asyncValue();
        }

        // Implements `then` where the lambda returns void. (Specialization of above method.)
        template<>
        void _then<void>(std::function<void(T&&)> callback) {
            if (ready())
                callback(extractResult());
            else
                Waiter::start(provider(), std::move(callback));
        }

        // Implements `then` where the lambda returns `Async<U>`.
        template <typename U>
        Async<U> _then(std::function<Async<U>(T&&)> callback) {
            if (ready()) {
                // If I'm ready, just call the callback and pass on the Async<U> it returns:
                return callback(extractResult());
            } else {
                // Otherwise wait for my result...
                auto uProvider = Async<U>::makeProvider();
                Waiter::start(provider(), [uProvider,callback=std::move(callback)](T&& result) {
                    // Invoke the callback, then wait to resolve the Async<U> it returns:
                    Async<U> u = callback(std::move(result));
                    u.then([uProvider](U &&uresult) {
                        // Then finally resolve the async I returned:
                        uProvider->setResult(std::move(uresult));
                    });
                });
                return uProvider->asyncValue();
            }
        }
    };


    //---- Implementation gunk...


    // Used by `BEGIN_ASYNC` macros. Returns the lexically enclosing actor instance, else NULL.
    // (How? Outside of an Actor method, `_thisActor()` refers to the function below.
    // In an Actor method, it refers to `Actor::_thisActor()`, which returns `this`.)
    static inline Actor* _thisActor() {return nullptr;}


    template <class T>
    Retained<AsyncProvider<T>> AsyncFnState::awaited() {
        // Move-returns `_awaiting`, downcast to the specific type of AsyncProvider<>.
        // The dynamic_cast is a safety check: it will throw a `bad_cast` exception on mismatch.
        (void)dynamic_cast<AsyncProvider<T>&>(*_awaiting);  // runtime type-check
        return reinterpret_cast<Retained<AsyncProvider<T>>&&>(_awaiting);
    }

    template <typename T>
    const T& AsyncProviderBase::result() {
        return dynamic_cast<AsyncProvider<T>*>(this)->result();
    }

    template <typename T>
    T&& AsyncProviderBase::extractResult() {
        return dynamic_cast<AsyncProvider<T>*>(this)->extractResult();
    }


    // Internal class used by `Async<T>::then()`, above
    template <typename T>
    class Async<T>::Waiter : public AsyncObserver {
    public:
        using Callback = std::function<void(T&&)>;

        static void start(AsyncProvider<T> *provider, Callback &&callback) {
            (void) new Waiter(provider, std::move(callback));
        }

    protected:
        Waiter(AsyncProvider<T> *provider, Callback &&callback)
        :_callback(std::move(callback))
        {
            provider->setObserver(this);
        }

        void asyncResultAvailable(Retained<AsyncProviderBase> ctx) override {
            auto provider = dynamic_cast<AsyncProvider<T>*>(ctx.get());
            _callback(provider->extractResult());
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
        friend class AsyncFnState;

        using Body = std::function<void(AsyncFnState&)>;

        AsyncProvider(Actor *actor, Body &&body)
        :AsyncProviderBase(actor)
        ,_body(std::move(body))
        { }

        AsyncProvider(Actor *actor, Body &&body, AsyncFnState &&state)
        :AsyncProvider(actor, std::move(body))
        {
            _fnState->updateFrom(state);
        }

        void setResult() {
            std::unique_lock<std::mutex> lock(_mutex);
            _gotResult(lock);
        }

        void asyncResultAvailable(Retained<AsyncProviderBase> async) override {
            assert(async == _fnState->_awaiting);
            _body(*_fnState);
            if (_fnState->_awaiting)
                _wait();
            else
                setResult();
        }

        Body _body;             // The async function body
    };


    // Specialization of Async<> for `void` type; not used directly.
    template <>
    class Async<void> : public AsyncBase {
    public:
        using AwaitReturnType = void;

//        static Retained<AsyncProvider<void>> makeProvider()  {return AsyncProvider<void>::create();}

        Async(AsyncProvider<void> *provider)                 :AsyncBase(provider) { }
        Async(const Retained<AsyncProvider<void>> &provider) :AsyncBase(provider) { }

        Async(Actor *actor, typename AsyncProvider<void>::Body bodyFn)
        :AsyncBase(new AsyncProvider<void>(actor, std::move(bodyFn)), true)
        { }
    };

}
