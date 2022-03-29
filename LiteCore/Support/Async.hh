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
#include "fleece/RefCounted.hh"
#include <atomic>
#include <cassert>
#include <functional>
#include <utility>

namespace litecore { namespace actor {
    class Actor;

    /*
     Async<T> represents a result of type T that may not be available yet. This concept is
     also referred to as a "future". You can create one by first creating an AsyncProvider<T>,
     which is also known as a "promise", then calling its `asyncValue` method:

        Async<int> getIntFromServer() {
            Retained<AsyncProvider<int>> intProvider = Async<int>::provider();
            sendServerRequestFor(intProvider);
            return intProvider->asyncValue();
        }

     You can simplify this somewhat:

        Async<int> getIntFromServer() {
            auto intProvider = Async<int>::provider();      // `auto` is your friend
            sendServerRequestFor(intProvider);
            return intProvider;                             // implicit conversion to Async
        }

     The AsyncProvider reference has to be stored somewhere until the result is available.
     Then you call its setResult() method:

        int result = valueReceivedFromServer();
        intProvider.setResult(result);

     Async<T> has a `ready` method that tells whether the result is available, and a `result`
     method that returns the result (or aborts if it's not available.) However, it does not
     provide any way to block and wait for the result. That's intentional: we don't want
     blocking! Instead, the way you work with async results is within an _asynchronous
     function_.

     ASYNCHRONOUS FUNCTIONS

     An asynchronous function is a function that can resolve Async values in a way that appears
     synchronous, but without actually blocking. It always returns an Async result (or void),
     since if the Async value it's resolving isn't available, the function itself has to return
     without (yet) providing a result. Here's what one looks like:

         Async<T> anAsyncFunction() {
            BEGIN_ASYNC_RETURNING(T)
            ...
            return t;
            END_ASYNC()
         }

     If the function doesn't return a result, it looks like this:

         void aVoidAsyncFunction() {
            BEGIN_ASYNC()
            ...
            END_ASYNC()
         }

     In between BEGIN and END you can "unwrap" Async values, such as those returned by other
     asynchronous functions, by calling asyncCall(). The first parameter is the variable to
     assign the result to, and the second is the expression returning the async result:

         asyncCall(int n, someOtherAsyncFunction());

     `asyncCall` is a macro that hides some very weird control flow. What happens is that, if
     the Async value isn't yet available, `asyncCall` causes the enclosing function to return.
     (Obviously it returns an unavailable Async value.) It also registers as an observer of
     the value, so when its result does become available, the enclosing function _resumes_
     right where it left off, assigns the result to the variable, and continues.

     ASYNC CALLS AND VARIABLE SCOPE

     `asyncCall()` places some odd restrictions on your code. Most importantly, a variable declared
     between the BEGIN/END cannot have a scope that extends across an `asyncCall`:

        int foo = ....;
        asyncCall(int n, someOtherAsyncFunction());     // ERROR: Cannot jump from switch...

     This is because the macro expansion of `asyncCall()` includes a `switch` label, and it's not
     possible to jump to that label (when resuming the async flow of control) skipping the variable
     declaration.

     If you want to use a variable across `asyncCall` scopes, you must declare it _before_ the
     BEGIN_ASYNC -- its scope then includes the entire async function:

        int foo;
        BEGIN_ASYNC_RETURNING(T)
        ...
        foo = ....;
        asyncCall(int n, someOtherAsyncFunction());     // OK!
        foo += n;

     Or if the variable isn't used after the next `asyncCall`, just use braces to limit its scope:

        {
            int foo = ....;
        }
        asyncCall(int n, someOtherAsyncFunction());     // OK!

     THREADING

     By default, an async method resumes immediately when the Async value it's waiting for becomes
     available. That means when the provider's `setResult` method is called, or when the
     async method returning that value finally returns a result. This is reasonable in single-
     threaded code.

     `asyncCall` is aware of Actors, however. So if an async Actor method waits, it will be resumed
     on that Actor's execution context. This ensures that the Actor's code runs single-threaded, as
     expected.

     */

#define BEGIN_ASYNC_RETURNING(T) \
    return _asyncBody<T>([=](AsyncState &_async_state_) mutable -> T { \
        switch (_async_state_.continueAt()) { \
            default:

#define BEGIN_ASYNC() \
    _asyncBody<void>([=](AsyncState &_async_state_) mutable -> void { \
        switch (_async_state_.continueAt()) { \
            default:

#define asyncCall(VAR, CALL) \
                if (_async_state_._asyncCall(CALL, __LINE__)) return {}; \
            case __LINE__: \
            VAR = _async_state_.asyncResult<decltype(CALL)::ResultType>(); _async_state_.reset();

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
        uint32_t continueAt() const                         {return _continueAt;}

        bool _asyncCall(const AsyncBase &a, int lineNo);

        template <class T>
        T&& asyncResult() {
            return ((AsyncProvider<T>*)_calling.get())->extractResult();
        }

        void reset()                                        {_calling = nullptr;}

    protected:
        fleece::Retained<AsyncContext> _calling;            // What I'm blocked awaiting
        uint32_t _continueAt {0};                           // label/line# to continue lambda at
    };


    // Maintains the context/state of an async operation and its observer.
    // Abstract base class of AsyncProvider<T>.
    class AsyncContext : public fleece::RefCounted, protected AsyncState {
    public:
        bool ready() const                                  {return _ready;}
        void setObserver(AsyncContext *p);
        void wakeUp(AsyncContext *async);

    protected:
        AsyncContext(Actor *actor);
        ~AsyncContext();
        void start();
        void _wait();
        void _gotResult();

        virtual void next() =0;

        bool _ready {false};                                // True when result is ready
        fleece::Retained<AsyncContext> _observer;           // Dependent context waiting on me
        Actor *_actor;                                      // Owning actor, if any
        fleece::Retained<Actor> _waitingActor;              // Actor that's waiting, if any
        fleece::Retained<AsyncContext> _waitingSelf;        // Keeps `this` from being freed

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
        explicit AsyncProvider(Actor *actor, const LAMBDA body)
        :AsyncContext(actor)
        ,_body(body)
        { }

        static fleece::Retained<AsyncProvider> create() {
            return new AsyncProvider;
        }

        Async<T> asyncValue() {
            return Async<T>(this);
        }

        void setResult(const T &result) {
            _result = result;
            _gotResult();
        }

        const T& result() const {
            assert(_ready);
            return _result;
        }

        T&& extractResult() {
            assert(_ready);
            return std::move(_result);
        }

    private:
        AsyncProvider()
        :AsyncContext(nullptr)
        { }

        void next() override {
            _result = _body(*this);
            if (_calling)
                _wait();
            else
                _gotResult();
        }

        std::function<T(AsyncState&)> _body;            // The async function body
        T _result {};                                   // My result
    };


    // Specialization of AsyncProvider for use in functions with no return value (void).
    template <>
    class AsyncProvider<void> : public AsyncContext {
    public:
        template <class LAMBDA>
        explicit AsyncProvider(Actor *actor, const LAMBDA &body)
        :AsyncContext(actor)
        ,_body(body)
        { }

        static fleece::Retained<AsyncProvider> create() {
            return new AsyncProvider;
        }

    private:
        AsyncProvider()
        :AsyncContext(nullptr)
        { }

        void next() override {
            _body(*this);
            if (_calling)
                _wait();
            else
                _gotResult();
        }

        std::function<void(AsyncState&)> _body;         // The async function body
    };


    // base class of Async<T>
    class AsyncBase {
    public:
        explicit AsyncBase(const fleece::Retained<AsyncContext> &context)
        :_context(context)
        { }

        bool ready() const                                          {return _context->ready();}

    protected:
        fleece::Retained<AsyncContext> _context;        // The AsyncProvider that owns my value

        friend class AsyncState;
    };


    /** An asynchronously-provided result, seen from the client side. */
    template <class T>
    class Async : public AsyncBase {
    public:
        using ResultType = T;

        Async(AsyncProvider<T> *provider)
        :AsyncBase(provider)
        { }

        Async(const fleece::Retained<AsyncProvider<T>> &provider)
        :AsyncBase(provider)
        { }

        template <class LAMBDA>
        Async(Actor *actor, const LAMBDA& bodyFn)
        :Async( new AsyncProvider<T>(actor, bodyFn) )
        {
            _context->start();
        }

        /** Returns a new AsyncProvider<T>. */
        static fleece::Retained<AsyncProvider<T>> provider() {
            return AsyncProvider<T>::create();
        }

        const T& result() const             {return ((AsyncProvider<T>*)_context.get())->result();}

        /** Invokes the callback when this Async's result becomes ready,
            or immediately if it's ready now. */
        template <class LAMBDA>
        void wait(LAMBDA callback) {
            if (ready())
                callback(result());
            else
                (void) new AsyncWaiter(_context, callback);
        }


        // Internal class used by wait(), above
        class AsyncWaiter : public AsyncContext {
        public:
            template <class LAMBDA>
            AsyncWaiter(AsyncContext *context, LAMBDA callback)
            :AsyncContext(nullptr)
            ,_callback(callback)
            {
                _waitingSelf = this;
                _calling = context;
                _wait();
            }

        protected:
            void next() override {
                _callback(asyncResult<T>());
                _waitingSelf = nullptr;
            }

        private:
            std::function<void(T)> _callback;
        };
    };


    // Specialization of Async<> for functions with no result
    template <>
    class Async<void> : public AsyncBase {
    public:
        Async(AsyncProvider<void> *provider)
        :AsyncBase(provider)
        { }

        Async(const fleece::Retained<AsyncProvider<void>> &provider)
        :AsyncBase(provider)
        { }

        static fleece::Retained<AsyncProvider<void>> provider() {
            return AsyncProvider<void>::create();
        }

        template <class LAMBDA>
        Async(Actor *actor, const LAMBDA& bodyFn)
        :Async( new AsyncProvider<void>(actor, bodyFn) )
        {
            _context->start();
        }
    };


    /** Body of an async function: Creates an AsyncProvider from the lambda given,
        then returns an Async that refers to that provider. */
    template <class T, class LAMBDA>
    Async<T> _asyncBody(const LAMBDA &bodyFn) {
        return Async<T>(nullptr, bodyFn);
    }

} }
