# The Async API

## Asynchronous Values (Futures)

`Async<T>` represents a value of type `T` that may not be available yet. This concept is also referred to as a ["future"](https://en.wikipedia.org/wiki/Futures_and_promises). 

You can create one by first creating an `AsyncProvider<T>`, which is also known as a "promise", then calling its `asyncValue` method:

```c++
Async<int> getIntFromServer() {
    Retained<AsyncProvider<int>> intProvider = Async<int>::provider();
    sendServerRequestFor(intProvider);
    return intProvider->asyncValue();
}
```

You can simplify this somewhat:

```c++
Async<int> getIntFromServer() {
    auto intProvider = Async<int>::provider();      // `auto` is your friend
    sendServerRequestFor(intProvider);
    return intProvider;                             // implicit conversion to Async
}
```

The `AsyncProvider` reference has to be stored somewhere until the result is available. Then you call its `setResult()` method:

```c++
int result = valueReceivedFromServer();
intProvider.setResult(result);
```

`Async<T>` has a `ready` method that tells whether the result is available, and a `result` method that returns the result (or aborts if it's not available.) However, it does not provide any way to block and wait for the result. That's intentional: we don't want blocking! Instead, the way you work with async results is within an _asynchronous function_.

## Asynchronous Functions

An asynchronous function is a function that can resolve `Async` values in a way that appears synchronous, but without actually blocking. It always returns an `Async` result (or void), since if the `Async` value it's resolving isn't available, the function itself has to return without (yet) providing a result.

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

In between `BEGIN_ASYNC` and `END_ASYNC` you can "unwrap" `Async` values, such as those returned by other asynchronous functions, by using the `asyncCall()` macro. The first parameter is the variable to assign the result to, and the second is the expression returning the async result:

```c++
asyncCall(int n, someOtherAsyncFunction());
```

> TMI: `asyncCall` is a macro that hides some very weird control flow. It first evaluates the second parameter to get its `Async` value. If that value isn't yet available, `asyncCall` _causes the enclosing function to return_. (Obviously it returns an unavailable `Async` value.) It also registers as an observer of the async value, so when its result does become available, the enclosing function _resumes_ right where it left off (🤯), assigns the result to the variable, and continues.

## Async Calls and Variables' Scope

The weirdness inside `asyncCall()` places some odd restrictions on your code. Most importantly, **a variable declared between `BEGIN_ASYNC()` and `END_ASYNC()` cannot have a scope that extends across an `asyncCall`**:

```c++
int foo = ....;
asyncCall(int n, someOtherAsyncFunction());     // ERROR: "Cannot jump from switch..."
```

> TMI: This is because the macro expansion of an async function wraps a `switch` statement around its body, and the expansion of `asyncCall()` contains a `case` label. The C++ language does not allow a jump to a `case` to skip past a variable declaration.

If you want to use a variable across `asyncCall` scopes, you must **declare it _before_ the `BEGIN_ASYNC`** -- its scope then includes the entire async function:

```c++
int foo;
BEGIN_ASYNC_RETURNING(T)
...
foo = ....;
asyncCall(int n, someOtherAsyncFunction());     // OK!
foo += n;
```

Or if the variable isn't used after the next `asyncCall`, just use braces to limit its scope:

```c++
{
    int foo = ....;
}
asyncCall(int n, someOtherAsyncFunction());     // OK!
```

## Threading

By default, an async method resumes immediately when the `Async` value it's waiting for becomes available. That means when the provider's `setResult` method is called, or when the async method returning that value finally returns a result, the waiting method will synchronously resume. This is reasonable in single- threaded code.

`asyncCall` is aware of [Actors](Actors.md), however. So if an async Actor method waits, it will be resumed on that Actor's execution context. This ensures that the Actor's code runs single-threaded, as expected.

