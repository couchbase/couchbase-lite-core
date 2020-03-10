# Actor School

## 1. What's An Actor?

An **Actor** is an object that implements the **[Actor Model](https://en.wikipedia.org/wiki/Actor_model)** of concurrency. Actors are externally opaque, and communicate only by **asynchronous message-passing**. Each actor has a **message queue** that serially processes the messages sent to it; thus, the implementation of an actor is **single-threaded** and requires no locking.

(If you're familiar with Apple's Grand Central Dispatch, you can think of an actor as being an object with its own serial dispatch queue, where all method calls on the object are dispatched asynchronously to the queue.)

### Good things about actors:

* The class implementation is simpler and easier to reason about since it's single-threaded. You never have to worry about concurrency issues like race conditions within a method, or use mutexes to access variables.
* Actors encourage encapsulation since they're so opaque.
* Actors are much cheaper than threads.
* Actors make good use of multi-core CPUs, because there's naturally a high degree of parallelism.

### Bad things about actors:

* Method calls between actors are asynchronous, so they can't return values. If you want to get a reply, you have to wait for the other actor to call you back, or pass a callback function in the message.
* Debugging code using actors can be challenging because of the asynchronous calls: you can't see the call stack past the current actor method. (Xcode's debugger can help with this, though.)
* Methods (in this implementation) are somewhat more verbose to write, because each consists of a public stub method and a private implementation method.
* You have to be careful with passing parameters by reference, so that the data being referenced doesn't get freed before the call completes, or used by the caller after the message is sent: otherwise it'd be shared and require locking. You need “move semantics” for such parameters.

## 2. What's This Library?

This is a C++11 implementation of the [Actor Model](https://en.wikipedia.org/wiki/Actor_model), or of something close to it. (Some may quibble that messaging doesn't work quite like usual.) It's small and lightweight. It should seem fairly idiomatic, since actor messages are regular C++ method calls. I won't claim it's bug-free, but it's been in use in Couchbase's mobile frameworks since 2017.

The “messages” in this library are C++ `function` objects that call private methods of the actor. An Actor subclass defines public methods that are inline and simply enqueue calls to private methods, with the same parameters. From the outside this looks like a normal C++ class, with the interesting behavior that all of its methods run asynchronously.

The implementation uses Apple's Grand Central Dispatch (GCD) on platforms where it's available, assigning a serial dispatch queue to each actor. Otherwise, each actor keeps a simple thread-safe queue of `std::function` objects; a global pool of `std::thread`s, one per CPU, pops these functions and calls them.

## 3. Implementing An Actor

To create an actor class, just subclass `litecore::actor::Actor`. There are no methods to override; Actor itself is a concrete, if do-nothing, class. It's recommended that you pass a name string to Actor's constructor, to give it a name; this helps with troubleshooting, as the name appears in logged messages.

Note that Actor is a subclass of `RefCounted`, so your actors must be implemented and used as proper ref-counted objects. (See the RefCounted class's documentation or header for details.)

To create a proper actor, you should adhere to these rules:

1. No mutable (non-const) data member may be public.
2. No public method [other than the constructor] may access any mutable data members, or call any other methods that do.

### Implementing methods

The above rules imply that the interesting methods must all be private (or at least protected.) So how can such a method be called from outside? By way of a public stub method, which merely calls `Actor::enqueue` to schedule a call to the internal method.

If you're playing by the rules, this implies that  *methods that access mutable state are only called from the actor's message queue*, whether directly or via another internal method. And since the message queue only calls one method at a time, that means your methods are single-threaded and don't have to worry about concurrency!

Here's a simple but functional actor class:

```c++
class Counter : public Actor {
public:
    Counter();
    void incrementBy(int n)     {enqueue(&Counter::_incrementBy, n);}
private:
    void _incrementBy(int n)    {_count += n;}
    int _count = 0;
};
```

Note the convention that the private implementation method has the same name as the public method, but with an underscore prefix.

The enqueue call is a bit weird looking. Its first parameter must be a pointer to the implementation method, a C++ “pointer to member function”. (Yes, it has to be qualified with the class name even though it's inside the class declaration; don't ask me why.) The remaining parameters are the parameters to be passed to the implementation method, which are *usually* the same as the parameters of the public method. If they're not the same, it's to wrap them up to protect them during the asynchronous call.

### Async Parameter Passing

As mentioned in the introduction, **you have to be careful passing parameters that involve references**. The most urgent issue is to ensure that the referenced memory isn't freed or altered before the async call completes. For example, passing a C string (a `const char*`) to `enqueue` is not recommended because the actor has no idea what the lifetime of the string's data is. If the caller frees or overwrites the string after the call, the `char*` is most likely going to point to garbage by the time the implementation method is called.


> What's going on under the hood is that the `enqueue` method calls `std::bind` to create a `std::function` object representing the call. This copies the parameters into the function object. If a parameter is a primitive type like `int`, or an object with value semantics like `string` or `vector`, that's sufficient to ensure it gets passed to the implementation intact. But if the parameter is a pointer or reference, C++ just copies the pointer, not the data itself.

### Copying Parameters

The way around this is by **copying** the data, usually by converting the parameter to a type with value semantics. In the example above, you'd probably use `std::string`. This is easily accomplished by making the implementation method take a `std::string` parameter; then the call to enqueue will implicitly convert the `char*` into a `std::string` object with a copy of the bytes.

```c++
class Appender : public Actor {
public:
    Appender();
    void append(const char *str)     {enqueue(&Appender::_append, str);}
private:
    void _append(std::string str)    {_str += str;}
    std::string _str;
};
```

### Reference Parameters

Sometimes we really do need to pass a reference, not a copy. A common case is when passing a reference to another actor. Here the problem is that we need to ensure the referenced object isn't freed before the implementation method is called. The best way is to ensure that the object is `RefCounted`, and then make the implementation parameter a `Retained<>` wrapper.

```c++
class MultiCounter : public Actor {
public:
    MultiCounter();
    void addCounter(Counter *c)            {enqueue(&MultiCounter::addCounter, c);}
private:
    void _addCounter(Retained<Counter> c)  {_counters.push_back(c);}
    std::vector<Retained<Counter>> _counters;
};
```

The danger of using reference parameters is that both the caller and the callee (the implementation method) will have references to the passed object. That means you're sharing data between threads, which can be dangerous! There are three ways to do this safely:

1. The object is entirely thread-safe (e.g. an actor)
2. The object is immutable
3. The object is **moved** to the callee: the caller destroys its reference to the object and never uses it again.

## 4. More Actor Tricks

### Delayed Calls

`Actor::enqueueAfter` is just like the regular `enqueue`, except that its first parameter is a time interval; the implementation method won't be called until that interval has elapsed.


> Watch out: `enqueueAfter` violates the usual invariant that an actor will perform methods in the same order that they were called. This can have unexpected results if you're not taking it into account.

### Returning Results, and Callbacks

Sometimes you need to get a result back from an actor method. You can't do this directly, because actor methods are async and return `void`. (Yes, there are such things as promises and futures, but this library doesn't have a fully-useable implementation of those yet.) Instead, you have to have the method call you back with the result. There are two ways to do this:

1. Pass a reference to yourself (`this`) and have the actor method call a known public method on that reference.
2. Pass a callback function, which the actor method will call.

These should be familiar if you've done multi-threaded programming before; they're the same answers as to “how do I have my background task call back to the main task when it's done?” The tricky part is that in either case the call is happening on whatever thread is running the actor's queue, so the method/function being called needs to be thread-safe and should not block.

Usually the caller is another actor. In that case you implement pattern 1 by making the called-back method an async one, i.e. a stub that just enqueues a call to the real implementation method. Pattern 2 is a bit harder: how do you make it so the lambda or function object will run on your event queue, if it's being called by another actor on its separate queue?

The answer is an Actor method called **`asynchronize`**. Its C++ declaration looks pretty cryptic, but what it does is this: it takes a lambda (or other callable value) as a parameter, and it returns a `function` object with the same parameters as the lambda. Calling that function object will *enqueue* an async call to the lambda, so it will run just as though it were an actor method.

Let's add a getter method to the Count class shown in the first example:

```c++
public:
    void getCount(std::function<void(int)> callback) {
        enqueue(&Counter::_getCount(callback));
    }
private:
    void _getCount(std::function<void(int)> callback) {
        callback(_count);
    }
```

After you call getCount, at some later time your callback will be invoked and passed the count. From a method in some other actor, you can safely call getCount like this:

```c++
counter->getCount(asynchronize([=](int count) {
    // do something with `count`...
}));
```

### Actor Info

**`eventCount`** returns the number of events in the actor's queue. This lets you tell whether the actor has work to do. The count includes any event that's currently running, so if called from an actor method, it always returns at least 1. (This method is public and thread-safe, although it's mostly useful only when called on `this`.)

**`actorName`** returns the name string given to the Actor constructor (if any.) Its use is up to you.

### Event Queue Utilities

**`afterEvent`** is a virtual method that's called by the event queue immediately after every actor method. It does nothing, but you can override it to perform housekeeping or update state. For instance, the Couchbase Lite replicator actors use it to recompute their busy/idle status and notify their parent object of changes to it.

**`caughtException`** is a virtual method that's called if the event queue catches a C++ exception thrown from an actor method (or asynchronized callback, or `afterEvent` method.) It's passed a reference to the exception as a `std::exception`. The default implementation logs a warning; you can override it to do your own error handling, but it's probably a good idea to call the inherited method.
`Actor::currentActor` is a static method that returns a pointer to the Actor currently running *on this thread*. In other words, if it's called from within an actor method or something called (directly) by an actor method, it will return that actor. Otherwise it returns null.

### Batcher

**`Batcher`** is a utility class template for use with actors. It helps implement a common use case, where an actor is given values to work on one at a time, but wants to process them in batches. (An example from Couchbase Lite is adding documents to a database: it's most efficient to add lots of documents in a single transaction.)

A Batcher is owned by an actor (probably as a data member) and initialized with a reference to that actor and with a pointer to one of its methods, called the “processor”. Its `push` method adds one item to the batch; it's thread-safe, so it can be called directly by a public method. Once the Batcher has an item, it enqueues an async call to the processor method. The processor method then calls `Batcher::pop`, which returns a vector of all the pushed items (and clears the Batcher), and it can then process all the items at once.

A Batcher can optionally have a latency and a capacity. If a latency is given, the Batcher will wait that long after the first item is pushed before enqueueing the call to the processor. However, if a capacity is also given, then if the number of items reaches the capacity first, the processor will be queued immediately.

Whether to use a latency and capacity, and what values to use, is a matter of fine-tuning. You'll often need to experiment, and the results can be counter-intuitive.

