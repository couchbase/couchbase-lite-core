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
#include <atomic>
#include <functional>
#include <optional>
#include <utility>
#include <betterassert.hh>

namespace litecore::actor {
    using fleece::RefCounted;
    using fleece::Retained;

    class Actor;


#define BEGIN_ASYNC_RETURNING(T) \
    return Async<T>(_enclosingActor(), [=](AsyncState &_async_state_) mutable -> std::optional<T> { \
        switch (_async_state_.currentLine()) { \
            default:

#define BEGIN_ASYNC() \
    Async<void>(_enclosingActor(), [=](AsyncState &_async_state_) mutable -> void { \
        switch (_async_state_.currentLine()) { \
            default:

#define asyncCall(VAR, CALL) \
                if (_async_state_._asyncCall(CALL, __LINE__)) return {}; \
            case __LINE__: \
            VAR = _async_state_.asyncResult<async_result_type<decltype(CALL)>>();\
            _async_state_.reset();

#define END_ASYNC() \
        } \
    });


    class AsyncBase;
    class AsyncContext;
    template <class T> class Async;
    template <class T> class AsyncProvider;


    /** The state data passed to the lambda of an async function. */
    class AsyncState {
    public:
        int currentLine() const                             {return _currentLine;}
        void reset()                                        {_awaiting = nullptr;}

        bool _asyncCall(const AsyncBase &a, int curLine);

        template <class T>
        T&& asyncResult() {
            return dynamic_cast<AsyncProvider<T>*>(_awaiting.get())->extractResult();
        }

    protected:
        Retained<AsyncContext> _awaiting;           // What my fn body is suspended awaiting
        int                    _currentLine {0};    // label/line# to continue lambda at
    };


#pragma mark - ASYNCPROVIDER:


    // Maintains the context/state of an async operation and its observer.
    // Abstract base class of AsyncProvider<T>.
    class AsyncContext : public RefCounted, protected AsyncState {
    public:
        bool ready() const                                  {return _ready;}
        void setObserver(AsyncContext *p);
        void wakeUp(AsyncContext *async);

    protected:
        AsyncContext(Actor*);
        ~AsyncContext();
        void _start();
        void _wait();
        void _gotResult();

        virtual void _next() =0;

        Retained<AsyncContext> _observer;                   // Dependent context waiting on me
        Actor*                 _actor;                      // Owning actor, if any
        Retained<Actor>        _waitingActor;               // Actor that's waiting, if any
        Retained<AsyncContext> _waitingSelf;                // Keeps `this` from being freed
        std::atomic<bool>      _ready {false};              // True when result is ready

#if DEBUG
    public:
        static std::atomic_int gInstanceCount;
#endif
        template <class T> friend class Async;
        friend class Actor;
    };


    /** An asynchronously-provided result, seen from the producer side. */
    template <class T>
    class AsyncProvider : public AsyncContext {
    public:
        template <class LAMBDA>
        explicit AsyncProvider(Actor *actor, LAMBDA body)
        :AsyncContext(actor)
        ,_body(std::move(body))
        { }

        static Retained<AsyncProvider> create()             {return new AsyncProvider;}

        Async<T> asyncValue()                               {return Async<T>(this);}

        void setResult(const T &result)                     {_result = result; _gotResult();}
        void setResult(T &&result)                          {_result = std::move(result); _gotResult();}

        const T& result() const                             {precondition(_result);return *_result;}
        T&& extractResult()                                 {precondition(_result);
                                                             return *std::move(_result);}
    private:
        AsyncProvider()                                     :AsyncContext(nullptr) { }

        void _next() override {
            _result = _body(*this);
            if (_awaiting) {
                _wait();
            } else {
                precondition(_result);
                _gotResult();
            }
        }

        std::function<std::optional<T>(AsyncState&)> _body;     // The async function body
        std::optional<T>                             _result;   // My result
    };


    // Specialization of AsyncProvider for use in functions with no return value (void).
    template <>
    class AsyncProvider<void> : public AsyncContext {
    public:
        template <class LAMBDA>
        explicit AsyncProvider(Actor *actor, LAMBDA body)
        :AsyncContext(actor)
        ,_body(std::move(body))
        { }

        static Retained<AsyncProvider> create()             {return new AsyncProvider;}

    private:
        AsyncProvider()                                     :AsyncContext(nullptr) { }

        void _next() override {
            _body(*this);
            if (_awaiting)
                _wait();
            else
                _gotResult();
        }

        std::function<void(AsyncState&)> _body;             // The async function body
    };


#pragma mark - ASYNC:


    // base class of Async<T>
    class AsyncBase {
    public:
        explicit AsyncBase(const Retained<AsyncContext> &c) :_context(c) { }
        bool ready() const                                  {return _context->ready();}
    protected:
        Retained<AsyncContext> _context;                    // The AsyncProvider that owns my value
        friend class AsyncState;
    };


    /** An asynchronously-provided result, seen from the client side. */
    template <class T>
    class Async : public AsyncBase {
    public:
        using ResultType = T;

        Async(AsyncProvider<T> *provider)                   :AsyncBase(provider) { }
        Async(const Retained<AsyncProvider<T>> &provider)   :AsyncBase(provider) { }

        template <class LAMBDA>
        Async(Actor *actor, LAMBDA bodyFn)
        :Async( new AsyncProvider<T>(actor, std::move(bodyFn)) )
        {
            _context->_start();
        }

        /// Returns a new AsyncProvider<T>.
        static Retained<AsyncProvider<T>> provider()        {return AsyncProvider<T>::create();}

        const T& result() const             {return ((AsyncProvider<T>*)_context.get())->result();}

        /// Invokes the callback when the result becomes ready, or immediately if it's ready now.
        template <class LAMBDA>
        void then(LAMBDA callback) {
            if (ready())
                callback(result());
            else
                (void) new AsyncWaiter(_context, std::move(callback));
        }

    private:
        // Internal class used by `then()`, above
        class AsyncWaiter : public AsyncContext {
        public:
            template <class LAMBDA>
            AsyncWaiter(AsyncContext *context, LAMBDA callback)
            :AsyncContext(nullptr)
            ,_callback(std::move(callback))
            {
                _waitingSelf = this; // retain myself while waiting
                _awaiting = context;
                _wait();
            }
        protected:
            void _next() override {
                _callback(asyncResult<T>());
                _callback = nullptr;
                _waitingSelf = nullptr; // release myself when done
            }
        private:
            std::function<void(T)> _callback;
        };
    };


    // Specialization of Async<> for functions with no result
    template <>
    class Async<void> : public AsyncBase {
    public:
        Async(AsyncProvider<void> *provider)                 :AsyncBase(provider) { }
        Async(const Retained<AsyncProvider<void>> &provider) :AsyncBase(provider) { }

        static Retained<AsyncProvider<void>> provider()      {return AsyncProvider<void>::create();}

        template <class LAMBDA>
        Async(Actor *actor, const LAMBDA& bodyFn)
        :Async( new AsyncProvider<void>(actor, bodyFn) )
        {
            _context->_start();
        }
    };


    /// Pulls the result type out of an Async type.
    /// If `T` is `Async<X>`, or a reference thereto, then `async_result_type<T>` is X.
    template <class T>
    using async_result_type = typename std::remove_reference_t<T>::ResultType;


    // Used by `BEGIN_ASYNC` macros. Returns the lexically enclosing actor instance, else NULL.
    // (How? Outside of an Actor method, `_enclosingActor()` refers to the function below.
    // In an Actor method, it refers to a method with the same name that returns `this`.)
    static inline Actor* _enclosingActor() {return nullptr;}

}
