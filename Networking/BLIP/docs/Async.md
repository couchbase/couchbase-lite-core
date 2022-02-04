# The Async API

(Last updated Feb 4 2022 by Jens)

## Asynchronous Values (Futures)

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

## Getting The Result With `then`

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

## Asynchronous Functions

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

The weird part is that “suspend” doesn’t actually mean “block the current thread.” Instead it temporarily returns from the function (giving the caller an Async value as a placeholder), but when the value becomes available it resumes the function where it left off. This reduces the need for multiple threads, and in an Actor it lets you handle other messages while the current one is suspended.

> TMI: `AWAIT` is a macro that hides some very weird control flow. It first evaluates the second parameter to get its `Async` value. If that value isn't yet available, `AWAIT` _causes the enclosing function to return_. (Obviously it returns an unavailable `Async` value.) It also registers as an observer of the async value, so when its result does become available, the enclosing function _resumes_ right at the line where it left off (🤯), assigns the result to the variable, and continues.

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

## Threading

By default, an async function starts immediately (as you’d expect) and runs until it either returns a value, or blocks in an AWAIT call on an Async value that isn’t ready. In the latter case, it returns to the caller, but its Async result isn’t ready yet.

When the `Async` value it's waiting for becomes available, the blocked function resumes *immediately*. That means: when the provider's `setResult` method is called, or when the async method returning that value finally returns a result, the waiting method will synchronously resume. 

### Async and Actors

These are reasonable behaviors in single- threaded code … not for [Actors](Actors.md), though. An Actors is single-threaded, so code belonging to an Actor should only run “on the Actor’s queue”, i.e. when no other code belonging to that Actor is running.

Fortunately, `BEGIN_ASYNC` and `AWAIT` are aware of Actors, and have special behaviors when the function they’re used in is a method of an Actor subclass:

* `BEGIN_ASYNC` checks whether the current thread is already running as that Actor. If not, it doesn’t start yet, but schedules the function on the Actor’s queue.
* When `AWAIT` resumes the function, it schedules it on the Actor's queue.

### Easier Actors Without Async

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

