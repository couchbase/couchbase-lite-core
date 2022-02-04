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
    return Async<T>(_thisActor(), [=](AsyncState &_async_state_) mutable -> std::optional<T> { \
        switch (_async_state_.currentLine()) { \
            default:

/// Put this at the top of an async method that returns `void`.
/// See `BEGIN_ASYNC_RETURNING` for details.
#define BEGIN_ASYNC() \
    Async<void>(_thisActor(), [=](AsyncState &_async_state_) mutable -> void { \
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
    class AsyncContext;
    template <class T> class Async;
    template <class T> class AsyncProvider;


    // The state data passed to the lambda of an async function. Internal use only.
    class AsyncState {
    public:
        int currentLine() const                             {return _currentLine;}
        void reset()                                        {_awaiting = nullptr;}
        bool _await(const AsyncBase &a, int curLine);
        template <class T> Retained<AsyncProvider<T>> awaited();

    protected:
        Retained<AsyncContext> _awaiting;           // What my fn body is suspended awaiting
        int                    _currentLine {0};    // label/line# to continue body function at
    };


#pragma mark - ASYNCPROVIDER:


    // Maintains the context/state of an async operation and its observer.
    // Abstract base class of AsyncProvider<T> and Async<T>::AsyncWaiter.
    class AsyncContext : public RefCounted, protected AsyncState,
                         public fleece::InstanceCountedIn<AsyncContext>
    {
    public:
        bool ready() const                                  {return _ready;}

        template <typename T> const T& result();
        template <typename T> T&& extractResult();

    protected:
        friend class AsyncBase;
        friend class Actor;

        explicit AsyncContext(Actor *actor = nullptr)       :_actor(actor) { }
        void _start();
        void _wait();
        void _waitOn(AsyncContext*);
        void _gotResult();

        virtual void _next();    // Overridden by AsyncProvider<T> and Async<T>::AsyncWaiter.

        Retained<AsyncContext> _observer;                   // Dependent context waiting on me
        Actor*                 _actor;                      // Owning actor, if any
        Retained<Actor>        _waitingActor;               // Actor that's waiting, if any
        Retained<AsyncContext> _waitingSelf;                // Keeps `this` from being freed
        std::atomic<bool>      _ready {false};              // True when result is ready

    private:
        void setObserver(AsyncContext *p);
        void wakeUp(AsyncContext *async);
    };


    /** An asynchronously-provided result, seen from the producer's side. */
    template <class T>
    class AsyncProvider : public AsyncContext {
    public:
        using ResultType = T;

        /// Creates a new empty AsyncProvider.
        static Retained<AsyncProvider> create()            {return new AsyncProvider;}

        static Retained<AsyncProvider> createReady(T&& r)  {return new AsyncProvider(std::move(r));}

        /// Creates the client-side view of the result.
        Async<T> asyncValue()                               {return Async<T>(this);}

        /// Resolves the value by storing the result and waking any waking clients.
        void setResult(const T &result)                     {precondition(!_result);
                                                             _result = result; _gotResult();}
        /// Resolves the value by move-storing the result and waking any waking clients.
        void setResult(T &&result)                          {precondition(!_result);
                                                             _result = std::move(result);
                                                             _gotResult();}

        /// Equivalent to `setResult` but constructs the T value directly inside the provider.
        template <class... Args,
                  class = std::enable_if<std::is_constructible_v<ResultType, Args...>>>
        void emplaceResult(Args&&... args) {
            precondition(!_result);
            _result.emplace(args...);
            _gotResult();
        }

        /// Returns the result, which must be available.
        const T& result() const &                           {precondition(_result);return *_result;}
        T&& result() &&                                     {return extractResult();}

        /// Moves the result to the caller. Result must be available.
        T&& extractResult()                                 {precondition(_result);
                                                             return *std::move(_result);}
    private:
        friend class Async<T>;

        using Body = std::function<std::optional<T>(AsyncState&)>;

        AsyncProvider() = default;

        explicit AsyncProvider(T&& result)
        :_result(std::move(result))
        {_ready = true;}

        AsyncProvider(Actor *actor, Body &&body)
        :AsyncContext(actor)
        ,_body(std::move(body))
        { }

        void _next() override {
            _result = _body(*this);
            assert(_result || _awaiting);
            AsyncContext::_next();
        }

        Body             _body;     // The async function body
        std::optional<T> _result;   // My result
    };



    // Specialization of AsyncProvider for use in functions with no return value (void).
    template <>
    class AsyncProvider<void> : public AsyncContext {
    public:
        static Retained<AsyncProvider> create()             {return new AsyncProvider;}

    private:
        friend class Async<void>;

        using Body = std::function<void(AsyncState&)>;

        AsyncProvider()                                     :AsyncContext(nullptr) { }

        AsyncProvider(Actor *actor, Body &&body)
        :AsyncContext(actor)
        ,_body(std::move(body))
        { }

        void _next() override {
            _body(*this);
            AsyncContext::_next();
        }

        Body _body;             // The async function body
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
        bool ready() const                                  {return _context->ready();}
    protected:
        friend class AsyncState;
        explicit AsyncBase(Retained<AsyncContext> &&context) :_context(std::move(context)) { }
        explicit AsyncBase(AsyncContext *context, bool);

        Retained<AsyncContext> _context;                    // The AsyncProvider that owns my value
    };


    /** An asynchronously-provided result, seen from the consumer's side. */
    template <class T>
    class Async : public AsyncBase {
    public:
        using ResultType = T;
        using AwaitReturnType = Async<T>;

        /// Returns a new AsyncProvider<T>.
        static Retained<AsyncProvider<T>> makeProvider()    {return AsyncProvider<T>::create();}

        Async(T&& t) :AsyncBase(AsyncProvider<T>::createReady(std::move(t))) { }
        Async(AsyncProvider<T> *provider)                   :AsyncBase(provider) { }
        Async(const Retained<AsyncProvider<T>> &provider)   :AsyncBase(provider) { }

        Async(Actor *actor, typename AsyncProvider<T>::Body bodyFn)
        :AsyncBase(new AsyncProvider<T>(actor, std::move(bodyFn)), true)
        { }

        /// Returns the result. (Will abort if the result is not yet available.)
        const T& result() const &                           {return _context->result<T>();}
        T&& result() const &&                               {return _context->result<T>();}

        /// Returns the result. (Will abort if the result is not yet available.)
        T&& extractResult() const                           {return _context->extractResult<T>();}

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

    private:
        class AsyncWaiter; // defined below

        // Implements `then` where the lambda returns a regular type `U`. Returns `Async<U>`.
        template <typename U>
        typename Async<U>::AwaitReturnType _then(std::function<U(T&&)> callback) {
            auto provider = Async<U>::provider();
            if (ready()) {
                provider->setResult(callback(extractResult()));
            } else {
                (void) new AsyncWaiter(_context, [provider,callback](T&& result) {
                    provider->setResult(callback(std::move(result)));
                });
            }
            return provider->asyncValue();
        }

        // Implements `then` where the lambda returns void. (Specialization of above method.)
        template<>
        void _then<void>(std::function<void(T&&)> callback) {
            if (ready())
                callback(extractResult());
            else
                (void) new AsyncWaiter(_context, std::move(callback));
        }

        // Implements `then` where the lambda returns `Async<U>`.
        template <typename U>
        Async<U> _then(std::function<Async<U>(T&&)> callback) {
            if (ready()) {
                // If I'm ready, just call the callback and pass on the Async<U> it returns:
                return callback(extractResult());
            } else {
                // Otherwise wait for my result...
                auto provider = Async<U>::makeProvider();
                (void) new AsyncWaiter(_context, [provider,callback](T&& result) {
                    // Invoke the callback, then wait to resolve the Async<U> it returns:
                    Async<U> u = callback(std::move(result));
                    u.then([=](U &&uresult) {
                        // Then finally resolve the async I returned:
                        provider->setResult(std::move(uresult));
                    });
                });
                return provider->asyncValue();
            }
        }
    };


    // Specialization of Async<> for `void` type; not used directly.
    template <>
    class Async<void> : public AsyncBase {
    public:
        using AwaitReturnType = void;

        static Retained<AsyncProvider<void>> makeProvider()  {return AsyncProvider<void>::create();}

        Async(AsyncProvider<void> *provider)                 :AsyncBase(provider) { }
        Async(const Retained<AsyncProvider<void>> &provider) :AsyncBase(provider) { }

        Async(Actor *actor, typename AsyncProvider<void>::Body bodyFn)
        :AsyncBase(new AsyncProvider<void>(actor, std::move(bodyFn)), true)
        { }
    };


    // Implementation gunk...


    // Used by `BEGIN_ASYNC` macros. Returns the lexically enclosing actor instance, else NULL.
    // (How? Outside of an Actor method, `_thisActor()` refers to the function below.
    // In an Actor method, it refers to `Actor::_thisActor()`, which returns `this`.)
    static inline Actor* _thisActor() {return nullptr;}


    template <class T>
    Retained<AsyncProvider<T>> AsyncState::awaited() {
        // Downcasts `_awaiting` to the specific type of AsyncProvider, and clears it.
        (void)dynamic_cast<AsyncProvider<T>&>(*_awaiting);  // runtime type-check
        return reinterpret_cast<Retained<AsyncProvider<T>>&&>(_awaiting);
    }

    template <typename T>
    const T& AsyncContext::result() {
        return dynamic_cast<AsyncProvider<T>*>(this)->result();
    }

    template <typename T>
    T&& AsyncContext::extractResult() {
        return dynamic_cast<AsyncProvider<T>*>(this)->extractResult();
    }

    // Internal class used by `Async<T>::then()`, above
    template <typename T>
    class Async<T>::AsyncWaiter : public AsyncContext {
    public:
        using Callback = std::function<void(T&&)>;

        AsyncWaiter(AsyncContext *context, Callback &&callback)
        :AsyncContext(nullptr)
        ,_callback(std::move(callback))
        {
            _waitOn(context);
        }
    protected:
        void _next() override {
            _callback(awaited<T>()->extractResult());
            _callback = nullptr;
            _waitingSelf = nullptr; // release myself when done
            // Does not call inherited method!
        }
    private:
        Callback _callback;
    };

}
