#  The Useful `Result<T>` Type

(Last updated March 4 2022 by Jens)

**Result is a utility class template for improving error handling without exceptions, inspired by languages like Swift, Kotlin and Rust.**

## 1. Why?

We still use exceptions inside LiteCore, but in some places it’s better to manage errors as `C4Error` values, usually when the error isn’t considered an “exceptional” situation where something’s gone unexpectedly wrong. An example of this is when saving a document — it’s entirely possible to get a kC4ErrorConflict in normal operation, so it shouldn’t be thrown. And in the case of asynchronous operations (`Async<T>`), exceptions don’t make sense at all.

In these situations we’ve been using the same calling convention we use in the C API: a special return value like `NULL` or `0` indicating failure, and a `C4Error*` parameter that the callee copies the error to. But that’s kind of awkward. We can do better.

## 2. What’s a `Result`?

`Result<T>`, defined in the header `Result.hh`, is **a container that can hold either a value of type `T` or a `C4Error`.** (It’s implemented using `std::variant`.)

- You can construct one from either a `T` or a `C4Error`.
- Boolean methods `ok()` and `isError()` tell you which it holds.
- `value()` returns the value … but if there’s an error it throws it instead. (So check first!)
- `error()` returns the error if there is one, or else a default error with `code==0`.

**Result’s main job is as the return value of a function that can fail:**

```c++
Result<double> squareRoot(double n) {
    if (n >= 0)
        return sqrt(n);
    else
        return C4Error{LiteCoreDomain, kC4ErrorInvalidParameter};
}
```

>  Note that one branch returns a `double` and the other a `C4Error`. That’s fine since they both convert implicitly to the actual return type, `Result<double>`.

### `Result<void>`

`Result<void>` is a special case where there isn’t any value to return, just “no error”. This subtype has no `value()` method, but you can otherwise treat it like other Results.

## 3. What Do You Do With One?

If you called a function that returned a Result, you can check what it holds and do the appropriate thing:

```c++
if (auto r = squareRoot(n); r.ok()) {
    cout << "√n = " << r.value() << endl;
} else {
    cerr << "squareRoot failed: " << r.error().description() << endl;
}
```

If you’re doing this inside a function that itself returns a `Result`, you can just pass the buck:

```c++
Result<void> showSquareRoot(double n) {
    if (auto r = squareRoot(n); r.ok()) {
        cout << "√n = " << r.value() << endl;
        return {};
    } else {
        return r.error();
    }
}
```

### CatchResult()

`CatchResult` lets you safely call a function that may throw an exception. Itakes a function/lambda that returns `T` (or `Result<T>`), calls it, and returns the result as a `Result<T>`. If the function throws an exception, it is caught and returned as the `error` in the result.

```c++
extern string read_line(stream*);  // throws exception on I/O error
Result<string> input = CatchResult( []{ return read_line(in); });
```

### then()

The `Result::then()` method lets you chain together operations that return Results. You can also look at it as a kind of functional “map” operation that transforms one type of Result into another.

It takes a function/lambda with a parameter of type `T` (or optimally, `T&&`) that returns some type `U`.

- If the receiver contains a value, it passes it to the function, then returns the function’s result wrapped in a `Result<U>`.
    - Bonus: if the function throws an exception, it’s caught and returned as the Result’s error.
- Otherwise, it just returns its error in a `Result<U>`.

Here `T` is `double`, `U` is `string`, and the function returns `U`:

```c++
Result<string> str = squareRoot(n).then( [](double root) {return to_string(root);} );
```

Here’s an example that goes the other direction, `string` to `double`, and the function returns a `Result`:

```c++
Result<double> root = parseDouble(str).then( [](double n) {return sqareRoot(n);} );
```

### onError()

The `onError` method is sort of the opposite of `then`: it takes a function/lambda and calls it with the _error_ value, if there is one; otherwise it does nothing.

`onError` returns itself (`*this`) since it hasn’t dealt with the non-error value and you need to do so. However, `Result<void>::onError` returns `void` because there’s no non-error value.

You can chain `then` and `onError` to handle both cases:

```c++
squareRoot(n).then( [](double root) {
    cout << "√n = " << root.value() << endl;
}).onError( [](C4Error error) {
    cerr << "squareRoot failed: " << error.description() << endl;
});
```

### TRY()

This one’s sort of an experiment to emulate Swift’s `try` statement (`try mightFail()`). Here’s an example:

```c++
Result<string> rootStr(double n) {
    TRY(double root, squareRoot(n));
    return std::to_string(root);
}
```

TRY calls `squareRoot(n)`; if it succeeds, it declares `root` and assigns the value to it. If it fails, though, it *returns the error from the enclosing function* (`rootStr`).

The `TRY` expression in the example is equivalent to:

```c++
auto __ = squareRoot(n);
if (__.isError()) return __.error();
double root = __.value();
```

> The syntax is ugly, but I think it’s the best that can be done in standard C++. (GCC and Clang have a “statement expression” extension that would let us say `double root = TRY(squareRoot(n));`, but MSVC doesn’t support it.)
