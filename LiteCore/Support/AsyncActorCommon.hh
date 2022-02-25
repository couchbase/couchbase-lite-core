//
// AsyncActorCommon.hh
//
// Copyright © 2022 Couchbase. All rights reserved.
//

#pragma once
#include <type_traits>

namespace litecore::actor {
    class Actor;
    template <typename T> class Async;


    /** Outside of an Actor method, `thisActor` evaluates to `nullptr`.
        (Inside of one, it calls the Actor method `thisActor` that returns `this`.) */
    static inline Actor* thisActor() {return nullptr;}


    // Compile-time utility that pulls the result type out of an Async type.
    // If `T` is `Async<X>`, or a reference thereto, then `async_result_type<T>` is X.
    template <class T>
    using async_result_type = typename std::remove_reference_t<T>::ResultType;


    // Magic template gunk. `unwrap_async<T>` removes a layer of `Async<...>` from a type:
    // - `unwrap_async<string>` is `string`.
    // - `unwrap_async<Async<string>> is `string`.
    template <typename T> T _unwrap_async(T*);
    template <typename T> T _unwrap_async(Async<T>*);
    template <typename T> using unwrap_async = decltype(_unwrap_async((T*)nullptr));

}
