# The Async API

(Last updated March 2 2022 by Jens)

**Async** is a major extension of LiteCore’s concurrency support, which should help us write clearer and safer multithreaded code in the future. It extends the functionality of Actors: so far, Actor methods have had to return `void` since they’re called asynchronously. Getting a value back from an Actor meant explicitly passing a callback function.

The Async feature allows Actor methods to return values; it’s just that those values are themselves asynchronous, and can’t be accessed by the caller until the Actor method finishes and returns a value. This may seem constraining, but it’s actually very useful. And any class can take advantage of Async values, not just Actors.

> If this sounds familiar: yes, this is an implementation of async/await as found in C#, JavaScript, Rust, etc. (C++ itself is getting this feature too, but not until C++20, and even then it’s only half-baked.)

## 1. Asynchronous Values (Futures & Promises)

`Async<T>` represents a value of type `T` that may not be available until some future time. This concept is also referred to as a ["future"](https://en.wikipedia.org/wiki/Futures_and_promises). You can keep it around like a normal value type, but you can’t get the underlying value until it becomes available. 

> You can think of `Async<T>` as sort of like `std::optional<T>`, except you can’t store a value in it yourself, only wait until something else does ... but what?

An `Async<T>` has a matching object, `AsyncProvider<T>`, that was created along with it and which belongs to whomever is responsible for producing that `T` value. (This is sometimes referred to as a “promise”.) The producer keeps it around, probably in a `Retained<>` wrapper, until such time as the result becomes available, then calls `setResult` on it to store the value into its matching `Async<T>`.

#### Example

Here’s a rather dumbed-down example of sending a request to a server and getting a response:

```c++
static Retained<AsyncProvider<int>> _curProvider;

Async<int> getIntFromServer() {
    _curProvider = Async<int>::makeProvider();
    sendServerRequest();
    return _curProvider->asyncValue();
}
```

Internally, this uses an `AsyncProvider<int>` reference to keep track of the current request (I told you this was dumbed down!), so when the response arrives it can store it into the provider and thereby into the caller’s `Async<int>` value:

```c++
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

## 2. Getting The Result With `then`

You can’t call `Async<T>::result()` until the result is available, or else Bad Stuff happens, like a fatal exception. We don’t want anything to block; that’s the point of async!

There’s a safe `ready()` method that returns `true` after the result is available. But obviously it would be a bad idea to do something like `while (!request.ready()) { }` …

So how do you wait for the result? **You don’t.** Instead you let the Async call _you_, by registering a callback. `Async`'s `then` method takes a lambda function that will be called with the result when it’s available:

```c++
Async<int> request = getIntFromServer();
request.then([=](int i) {
    std::cout << "The result is " << i << "!\n";
}, assertNoError);
```

> (What’s that `assertNoError`? Ignore it for now; it’ll be explained in the error handling section.)

What if you need that lambda to return a value? That value won’t be available until later when the lambda runs, so it too is returned as an `Async`:

```c++
Async<string> message = getIntFromServer().then([=](int i) {
    return "The result is " + std::stoi(i) + "!";
});
```

This works even if the inner lambda itself returns an `Async`:

```c++
extern Async<Status> storeIntOnServer(int);

Async<Status> status = getIntFromServer().then([=](int i) {
    return storeIntOnServer(i + 1);
});
```

In this situation it can be useful to **chain multiple `then` calls:**

```c++
Async<string> message = getIntFromServer().then([=](int i) {
    return storeIntOnServer(i + 1);
}).then([=](Status s) {
    return status == Ok ? "OK!" : "Failure";
});
```

### Async with no value (`Async<void>`)

Sometimes an asynchronous operation doesn’t need to return a value, but you still want to use `Async` with it so callers can be notified when it finishes. For that, use `Async<void>`. For example:

```c++
Async<void> slowOperation() {
       _curProvider = Async<void>::makeProvider();
    startOperationInBackground();
    return _curProvider->asyncValue();
}

static void operationFinished() {
    _curProvider.setResult(kC4NoError);
    _curProvider = nullptr;
}
```

Since there’s no actual result, you store a no-error value in the provider to indicate that it’s done.

Similarly with a `then` call — if your callback returns nothing (`void`), the result will be an `Async<void>` that merely indicates the completion of the callback:

```c++
Async<void> done = getIntFromServer().then([=](int i) {
    _currentInt = i;
});
```

### Be Careful With Captures!

It’s worth repeating the usual warnings about lambdas that can be called after the enclosing scope returns: **don’t capture by reference** (don’t use `[&]`) and **don’t capture pointers or `slice`s**. Here C++14’s capture-initializer syntax can be helpful:

```c++
slice str = .....;                               // slices are not safe to capture!
somethingAsync().then([str = alloc_slice(str)]   // capture `str` as `alloc_slice`
                      (int i) { ... });
```

One remaining problem is that **you can’t capture uncopyable types, like unique_ptr**. If you try you’ll get strange error messages from down in the standard library headers. The root problem is that `std::function` is copyable, so it requires that lambdas be copyable, which means their captured values have to be copyable. Facebook’s *folly* library has an alternative [Function](https://github.com/facebook/folly/blob/main/folly/docs/Function.md) class that avoids this problem, so if this turns out to be enough of a headache we could consider adopting that.

## 3. Async and Actors

### Running `then` on the Actor’s Thread

By default, a `then()` callback is called immediately when the provider’s `setResult()` is called, i.e. on the same thread the provider is running on.

But Actors want everything to run on their thread. For that case, `Async` has an `on(Actor*)` method that lets you specify that a subsequent `then()` should schedule its callback on the Actor’s thread.

```c++
Async<void> MyActor::downloadInt() {
    return getIntFromServer() .on(this) .then([=](int i) {
        // This code runs on the Actor's thread
        _myInt = i;
    });
}
```

### Implementing Async Actor Methods

A public method of an Actor can be called on any thread, so normally it just enqueues a call to the real method, which is private and (by convention) has an “_” prefix on its name:

```c++
class MyActor : public Actor {
public:
    void start() { enqueue(FUNCTION_TO_QUEUE(MyActor::_start)); }
private:
    void _start() { /* This code runs on the Actor's thread */ }
```

A new addition to Actor provides a way to do this without having to have two methods. It also makes it easy to return an Async value from an Actor method. All you do is wrap the body of the method in a lambda passed to `asCurrentActor()`:

```c++
class MyActor : public Actor {
public:
    void start() {
        return asCurrentActor([=] { /* This code runs on the Actor's thread */ });
    }
    
    Async<Status> getStatus() {
        return asCurrentActor([=] {
            // This code runs on the Actor's thread
            return _status;
        });
    }
```

As a bonus, if `asCurrentActor` is called on the Actor’s thread, it just calls the function immediately without enqueuing it, which is faster.

# Exceptions & C4Errors

### Providing an Error Result

Any Async value (regardless of its type parameter) can resolve to an error instead of a result. You can store one by calling `setError(C4Error)` on the provider. 

 If the code producing the value throws an exception, you can catch it and set it as the result with `setError()`.

```c++
try {
    ...
    provider->setResult(result);
} catch (const std::exception &x) {
    provider->setError(x));
}
```

> Note: `asCurrentActor()` catches exceptions thrown by its lambda and returns them as an error on the returned Async.

### The `Result` class

By the way, as part of implementing this, I added a general purpose `Result<T>` class (see `Result.hh`.) This simply holds either a value of type `T` or a `C4Error`. It’s similar to types found in Swift, Rust, etc.

```c++
Result<double> squareRoot(double n) {
    if (n >= 0)
        return sqrt(n);
    else
        return C4Error{LiteCoreDomain, kC4ErrorInvalidParameter};
}

if (auto root = squareRoot(x); root.ok())
    cout << "√x = " << root.value() << endl;
else
    cerr << "No square root: " << root.error().description() << endl;
```



### Handling An Error

The regular `then` methods described earlier can’t tell their callback about an error, because their callbacks take a parameter of type `T`. So what happens if the result is an error? Consider this example from earlier:

```c++
Async<Status> incrementIntOnServer() {
    return getIntFromServer().then([=](int i) {
    	return storeIntOnServer(i + 1);
	});
}
```

What happens if the async result of `getIntFromServer()` is an error? **The callback lambda is not called.** Instead, the error value is propagated to the `Async<Status>` , basically “passing the buck” to the caller of `incrementIntOnServer`. This is usually what you want.

If you want to handle the result whether or not it’s an error, you can set the callback’s parameter type to `Result<T>`:

```c++
getIntFromServer().then([=](Result<int> i) {
    if (i.ok())
        _latestInt = i.value();
    else
        cerr << "Couldn't get int: " << i.error().description() << endl;
});
```

Note that this form of `then()` does not return any value, because the callback completely handles the operation.

Another way to do this is to pass **two callbacks** to `then`:

```c++
getIntFromServer().then([=](int i) {
        _latestInt = i.value();
}, [=](C4Error error) {
    cerr << "Couldn't get int: " << error.description() << endl;
});
```

This finally explains the reason for mysterious `assertNoError` in the first example of section 2: that’s a function declared in `Async.hh` that simply takes a `C4Error` and throws an exception if it’s non-zero. That example was calling this two-callback version of `then` but idiomatically asserting that there would be no error.

### Returning an error from a `then` callback

There are two ways that a `then` method’s callback can signal an error.

1. The callback can throw an exception. This will be caught and converted into a `C4Error` result.
2. The callback can return a `C4Error` directly, by explicitly declaring a return type of `Result<T>`. This works because `Result` can be initialized with either a value or an error.

Here’s an example of the second form:

```c++
Async<double> squareRootFromServer() {
    return getIntFromServer().then([=](int i) -> Result<double> { // explicit result type!
        if (i >= 0)
            return sqrt(i);
        else
            return C4Error{LiteCoreDomain, kC4ErrorRemoteError};
	});
}
```

## Appendix: Design

First off, I’m aware that C++ already has a `std::future` class. However, it uses blocking control flow: 

> The `get` member function waits until the `future` has a valid result and (depending on which template is used) retrieves it. It effectively calls `wait()` in order to wait for the result. ([\*](https://en.cppreference.com/w/cpp/thread/future/get))

`std::future` has no mechanism to observe the result or register a callback. This makes it unusable in our async-oriented concurrency system.

The “async/await” mechanism that’s now available in many languages was an inspiration, but unfortunately it’s not possible to implement something like `await` in C++17 — it changes control flow fundamentally, turning the enclosing function into a coroutine. I did try to implement this using some [weird C(++) tricks](https://www.chiark.greenend.org.uk/~sgtatham/coroutines.html) and macros, but it was too inflexible and had too many broken edge cases. We’ll have to wait until we can move to [C++20](https://en.cppreference.com/w/cpp/language/coroutines).

You *can* do async without `await`; it just needs a “`then({...})`” idiom of chaining callback handlers. JavaScript did this before the `await` syntax was added in 2017.

Two C++ libraries I took design ideas from were [Folly](https://engineering.fb.com/2015/06/19/developer-tools/futures-for-c-11-at-facebook/) (by Facebook) and [Cap’n Proto](https://github.com/capnproto/capnproto/blob/master/kjdoc/tour.md#asynchronous-event-loop). I went with a different class name, though; Folly calls them `future<T>` and Cap’n Proto calls them `Promise<T>`. I just liked `Async<T>` better. `¯\_(ツ)_/¯` 

I didn’t directly use either library because their async code is tied to their own implementations of event loops, while we need to tie in with our existing `Actor` and `Mailbox`.
