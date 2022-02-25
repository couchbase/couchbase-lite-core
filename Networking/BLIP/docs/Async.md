# The Async API

(Last updated Feb 24 2022 by Jens)

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
    return intProvider->asyncValue();
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
});
```

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

In this situation it can be useful to chain multiple `then` calls:

```c++
Async<string> message = getIntFromServer().then([=](int i) {
    return storeIntOnServer(i + 1);
}).then([=](Status s) {
    return status == Ok ? "OK!" : "Failure";
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
void MyActor::downloadInt() {
    getIntFromServer() .on(this) .then([=](int i) {
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

# Exceptions

Any Async value (regardless of its type parameter) can hold an exception. If the code producing the value fails with an exception, you can catch it and set it as the result with `setException`.

```c++
try {
    ...
    provider->setResult(result);
} catch (...) {
    provider->setException(std::current_exception());
}
```

