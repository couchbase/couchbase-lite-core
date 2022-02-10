# The Async API

(Last updated Feb 7 2022 by Jens)

**Async** is a major extension of LiteCore’s concurrency support, which should help us write clearer and safer multithreaded code in the future. It extends the functionality of Actors: so far, Actor methods have had to return `void` since they’re called asynchronously. Getting a value back from an Actor meant explicitly passing a callback function.

The Async feature allows Actor methods to return values; it’s just that those values are themselves asynchronous, and can’t be accessed by the caller until the Actor method finishes and returns a value. This may seem constraining, but it’s actually very useful. And any class can take advantage of Async values, not just Actors.

> If this sounds familiar: yes, this is an implementation of async/await as found in C#, JavaScript, Rust, etc. (C++ itself is getting this feature too, but not until C++20, and even then it’s only half-baked.)

## 1. Asynchronous Values (Futures)

`Async<T>` represents a value of type `T` that may not be available yet. This concept is also referred to as a ["future"](https://en.wikipedia.org/wiki/Futures_and_promises). You can keep it around like a normal value type, but you can’t get the underlying value until it becomes available. 

> You can think of `Async<T>` as sort of like `std::optional<T>`, but you can’t store a value in it yourself, only wait until something else (the *producer*) does.

An `Async<T>` has a matching object, `AsyncProvider<T>`, that belongs to whomever is responsible for creating that `T` value. (This is sometimes referred to as a “promise”.) The producer keeps it around, probably in a `Retained<>` wrapper, until such time as the result becomes available, then calls `setResult` on it to store the value into its matching `Async<T>`.

#### Example

Here’s a rather dumbed-down example of sending a request to a server and getting a response:

```c++
Async<int> getIntFromServer() {
    _curProvider = Async<int>::makeProvider();
    sendServerRequest();
    return intProvider->asyncValue();
}
```

Internally, this uses an `AsyncProvider<int>` reference to keep track of the current request (I told you this was dumbed down!), so when the response arrives it can store it into the provider and thereby into the caller’s `Async<int>` value:

```c++
static Retained<AsyncProvider<int>> _curProvider;

static void receivedResponseFromServer(int result) {
	_curProvider.setResult(result);
    _curProvider = nullptr;
}
```

On the calling side you can start the request, go on your merry way, and then later get the value once it’s ready:

```c++
Async<int> request = getIntFromServer();  // returns immediately!
//... do other stuff ...

//... later, when the response is available:
int i = request.result();
cout << "Server says: " << i << "!\n";
```

Only … when is “later”, exactly? How do you know?

### Getting The Result With `then`

You can’t call `Async<T>::result()` before the result is available, or Bad Stuff happens, like a fatal exception. We don’t want anything to block; that’s the point of async!

There’s a safe `ready()` method that returns `true` after the result is available. But obviously it would be a bad idea to do something like `while (!request.ready()) { }` …

So how do you wait for the result? **You don’t.** Instead you let the Async call you, by calling its `then` method to register a lambda function that will be called with the result when it’s available:

```c++
Async<int> request = getIntFromServer();
request.then([](int i) {
    std::cout << "The result is " << i << "!\n";
});
```

What if you need that lambda to return a value? That value won’t be available until later when the lambda runs, but you can get it now as an `Async`:

```c++
Async<string> message = getIntFromServer().then([](int i) {
    return "The result is " + std::stoi(i) + "!";
});
```

This works even if the inner lambda itself returns an `Async`:

```c++
extern Async<Status> storeIntOnServer(int);

Async<Status> message = getIntFromServer().then([](int i) {
    return storeIntOnServer(i + 1);
});
```

In this situation it can be useful to chain multiple `then` calls:

```c++
Async<string> message = getIntFromServer().then([](int i) {
    return storeIntOnServer(i + 1);
}).then([](Status s) {
    return status == Ok ? "OK!" : "Failure";
});
```

## 2. Asynchronous Functions

An asynchronous function is a function that can resolve `Async` values in a way that *appears* synchronous, but without actually blocking. It lets you write code that looks more linear, without a bunch of “…then…”s in it. The bad news is that it’s reliant on some weird macros that uglify your code a bit.

An async function always returns an `Async` result, or void, since if an `Async` value it's resolving isn't available, the function itself has to return without (yet) providing a result.

> This is very much modeled on the "async/await" feature found in many other languages, like C#, JavaScript and Swift. C++20 has it too, under the name "coroutines", but we can't use C++20 yet.

Here's what an async function looks like:

```c++
 Async<T> anAsyncFunction() {
    BEGIN_ASYNC_RETURNING(T)
    ...
    return t;
    END_ASYNC()
 }
```

If the function doesn't return a result, and a caller won't need to know when it finishes, it doesn't need a return value at all, and looks like this:

```c++
void aVoidAsyncFunction() {
    BEGIN_ASYNC()
    ...
    END_ASYNC()
}
```

In between `BEGIN_ASYNC` and `END_ASYNC` you can "unwrap" `Async` values, such as those returned by other asynchronous functions, by using the `AWAIT()` macro. The first parameter is the variable to assign the result to, and the second is the expression returning the async result:

```c++
AWAIT(n, someOtherAsyncFunction());
```

This means “call `someOtherAsyncFunction()` [which returns an `Async`], *suspend this function* until that `Async`’s result becomes available, then assign the result to `n`.”

The weird part is that “suspend” doesn’t actually mean “block the current thread.” Instead it *temporarily returns* from the function (giving the caller an Async value as a placeholder), but when the value becomes available it *resumes* the function where it left off. This reduces the need for multiple threads, and in an Actor it lets you handle other messages while the current one is suspended.

> TMI: `AWAIT` is a macro that hides some very weird control flow. It first evaluates the second parameter to get its `Async` value. If that value isn't yet available, `AWAIT` _causes the enclosing function to return_. (Obviously it returns an unavailable `Async` value.) It also registers as an observer of the async value, so when its result does become available, the enclosing function _resumes_ right at the line where it left off (🤯), assigns the result to the variable, and continues. If you want even more gory details, see the appendix.

### Parameters Of ASYNC functions

You need to stay aware of the fact that an async function can be suspended, either in the `BEGIN_ASYNC()` or in an `AWAIT()`. One immediate consequence is that **the function shouldn’t take parameters that are pointers or references**, or things that behave like them (notably `slice`), because their values are likely to be garbage after the function’s been suspended and resumed. The same goes for any local variables in the function that are used across suspension points.

| Unsafe                     | Safe                                 |
| -------------------------- | ------------------------------------ |
| `MyRefCountedClass*`       | `Retained<MyRefCountedClass>`        |
| `MyStruct*` or `MyStruct&` | `MyStruct` or `unique_ptr<MyStruct>` |
| `slice`                    | `alloc_slice`                        |
| `string_view`              | `string`                             |

(I feel compelled to admit that it is actually safe to have such parameters … as long as you use them *only before* the `BEGIN_ASYNC` call, not after. This seems like a rare case, though; it might happen in a method that can usually return immediately but only sometimes needs to go the async route.)

### Variable Scope Inside ASYNC functions

The weirdness inside `AWAIT()` places some restrictions on your code. Most importantly, **a variable declared between `BEGIN_ASYNC()` and `END_ASYNC()` cannot have a scope that extends across an `AWAIT`**:

```c++
BEGIN_ASYNC()
int foo = ....;
AWAIT(int n, someOtherAsyncFunction());     // ERROR: "Cannot jump from switch..."
```

> TMI: This is because the macro expansion of an async function wraps a `switch` statement around its body, and the expansion of `AWAIT()` contains a `case` label. The C++ language does not allow a jump to a `case` to skip past a variable declaration.

If you want to use a variable across `AWAIT` calls, you must **declare it _before_ the `BEGIN_ASYNC`** -- its scope then includes the entire async function:

```c++
int foo;
BEGIN_ASYNC()
...
foo = ....;
AWAIT(int n, someOtherAsyncFunction());     // OK!
foo += n;
```

Or if the variable isn't used after the next `AWAIT`, just use braces to limit its scope:

```c++
BEGIN_ASYNC()
{
    int foo = ....;
}
AWAIT(int n, someOtherAsyncFunction());     // OK!
```

## 3. Threading and Actors

By default, an async function starts immediately (as you’d expect) and runs until it either returns a value, or blocks in an AWAIT call on an Async value that isn’t ready. In the latter case, it returns to the caller, but its Async result isn’t ready yet.

When the `Async` value it's waiting for becomes available, the blocked function resumes *immediately*. That means: when the provider's `setResult` method is called, or when the async method returning that value finally returns a result, the waiting method will synchronously resume. 

### Async and Actors

These are reasonable behaviors in single- threaded code … not for [Actors](Actors.md), though. An Actors is single-threaded, so code belonging to an Actor should only run “on the Actor’s queue”, i.e. when no other code belonging to that Actor is running.

Fortunately, `BEGIN_ASYNC` and `AWAIT` are aware of Actors, and have special behaviors when the function they’re used in is a method of an Actor subclass:

* `BEGIN_ASYNC` checks whether the current thread is already running as that Actor. If not, it doesn’t start yet, but schedules the function on the Actor’s queue.
* When `AWAIT` resumes the function, it schedules it on the Actor's queue.

### Easier Actors With Async

One benefit of this is that Actor methods using `BEGIN/END_ASYNC` don’t need the usual idiom where the public method enqueues a call to a matching private method:

```c++
// The old way ... In Twiddler.hh:
class Twiddler : public Actor {
public:
    void twiddle(int n) {enqueue(FUNCTION_TO_QUEUE(Twiddler::_twiddle), n);}
private:
    void _twiddle(int n);
};

// The old way ... In Twiddler.cc:
void Twiddler::_twiddle(int n) { 
    ... actual implementation ... 
}
```

Instead, BEGIN_ASYNC lets the public method enqueue itself and return, without the need for a separate method:

```c++
// The new way ... In Twiddler.hh:
class Twiddler : public Actor {
public:
    void twiddle(int n);
};

// The new way ... In Twiddler.cc:
void Twiddler::twiddle(int n) {
    BEGIN_ASYNC()
    ... actual implementation ...
    END_ASYNC()
}
```

## 4. Appendix: How Async Functions Work

The weird part of async functions is the way the `AWAIT()` macro can *suspend* the function in the middle, and then *resume* it later when the async value is ready. This seems like black magic until you find out hot it works; it’s actually a clever technique for implementing coroutines in C invented by [Simon Tatham](https://www.chiark.greenend.org.uk/~sgtatham/coroutines.html), which is based on an earlier hack called [Duff’s Device](https://en.wikipedia.org/wiki/Duff%27s_device), a weird (ab)use of the `switch` statement.

Let’s look at a simple async function that calls another async function:

```c++
Async<string> provideSum();

Async<string> provideSumPlus() {
    string a;
    BEGIN_ASYNC_RETURNING(string)       // (a)
    Log("Entering provideSumPlus");
    AWAIT(a, provideSum());             // (b)
    return a + "!";
    END_ASYNC()                         // (c)
}
```

If we run this through the preprocessor, we get:

```c++
Async<string> provideSumPlus() {
    string a;
    return Async<string>(_thisActor(), [=](AsyncState &_async_state_) mutable         // (a)
                                                -> std::optional<string> {            //
        switch (_async_state_.currentLine()) {                                        //
            default:                                                                  //
    			Log("Entering provideSumPlus");
                if (_async_state_._await(provideSum(), 78)) return {};                // (b)
            case 78:                                                                  //
                a = _async_state_.awaited<async_result_type<decltype(provideSum())>>()//
                                    ->extractResult();                                //
                return a + "!";
        }                                                                             // (c)
    });                                                                               //
}
```

`BEGIN_ASYNC_RETURNING(string)` turns into `return Async<string>(...)`, where the constructor parameters are `_thisActor()`, and a lambda that contains the rest of the function wrapped in a `switch` statement.

`_thisActor()` is simple: within the scope of an Actor method, it’s an inline method that returns `this`. Otherwise, it’s a global function that returns `nullptr`. So this call evaluates to the Actor implementing this function/method, if any.

The `Async<string>` constructor either calls the lambda immediately or (if it’s in an Actor method) schedules it to be called on the Actor’s queue. The function ends up returning this Async instance, whether or not it’s been resolved.

The really interesting stuff happens in that lambda. It’s passed an `AsyncState` value, which stores some state while the function is suspended; one piece of state is *which line of code it suspended at*, initially 0.

1. The first thing the lambda does is enter a `switch` on the state’s `currentLine()` value. This will of course jump to the corresponding label. The first time the lambda is called, `currentLine()` is 0, so…
2. The PC jumps to the `default:` label, which is right at the start of the code we wrote.
3. The function writes to the log, then hits the magic AWAIT call, which has turned into:
4. `provideSum()` is called. This returns an `Async<string>` value.
5. We call `_async_state_.await()` and pass it this Async value and the integer 78, which happens to be the current source line, as produced by the `__LINE__` macro. This function stores the value and line number into the AsyncState. The value returned is false if the Async has a result already, true if it doesn’t. Let’s assume the latter, since it’s more interesting.
6. Back in `provideSumPlus()`, the `false` result causes the lambda to return early. The lambda’s return type is `optional<string>`, so the return value is `nullopt`.
7. The caller of the lambda (never mind who) sees that it’s unfinished since it returned `nullopt`. So it gets put into the suspended state. A listener is added to the Async value it’s waiting on, so that when that Async’s result arrives, the lambda will be called again.
8. After the result arrives, the lambda restarts. This time, `_async_state_.currentLine()` returns 78 (stored into it in step 5), so the `switch` statement jumps to `case 78`.
9. The next line is messy due to some template gunk. `_async_state_.awaited<...>()` returns the AsyncProvider of the Async that we were waiting for, and `extractResult()` gets the result from that provider. That value is assigned to our variable `a`.
10. Now we’re back in regular code. We simply append `“!”` to `a` and return that.
11. This time the caller of the lambda gets a real value and knows the function is done. It stuffs that value into the result of the Async<string> it created way at the start (the one the top-level function returned) and notifies listeners that it’s available.
12. At this point the Async value returned by provideSumPlus() has a result, and whatever’s awaiting that result can run.
