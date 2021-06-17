# The LiteCore C++ API

5 April 2021

The API in this directory (`C/Cpp_include/`) gives the same full access to LiteCore’s functionality as does the primary C API; but it’s fully idiomatic C++. 

All you have to do to use it is include “`.hh`” files instead of “`.h`”, e.g.
	#include "c4Database.hh"
instead of
	#include "c4Database.h"

## Benefits

* The API “classes” `C4Database`, etc. are actual C++ classes. (Well, they’re declared as `struct` for compatibility, but that makes no difference.) So you can call `doc->selectNextRevision()` instead of `c4doc_selectNextRevision(doc)`.
* Most of the classes inherit from `fleece::RefCounted` and can and should be managed using the `Retained<T>` smart-pointer, which takes care of the ref-counting so you never have to remember to release a returned object pointer.
* Errors are thrown as C++ exceptions, so the “`C4Error *outError`” parameters littering the C API are gone. (See below for more on error handling.)
* The API uses `slice` and `alloc_slice`, which are more convenient to use in C++ than `C4Slice` and `C4SliceResult`. In particular, returned `alloc_slice`s are automatically cleaned up.
* Some methods take or return `std::vector` or `std::unordered_map` instead of awkwardly-specified C arrays.
* Your code will be slightly smaller, since it doesn’t have to bother with checking errors on every call. (This assumes your code is already built with C++ exceptions enabled. If not, turning on exceptions will inevitably add overhead.)
* Your code will be slightly faster, since it’s calling directly into LiteCore instead of detouring through a layer of C compatibility (see below.)

## Compatibility With The C API

The two APIs are pretty compatible. This is because **the C API is actually implemented as a thin wrapper around the C++ API**. The “object” references are compatible — opaque struct pointers in C are bona fide object pointers in C++. The C functions mostly just call methods, convert some parameter/result types, and catch exceptions. 

Here’s an example. Given this imaginary method `C4Foo::bar()` from the C++ API:
	struct C4Foo : public RefCounted {
	    ...
	    alloc_slice bar(slice input = nullslice);

The corresponding C function `c4foo_bar()` is implemented as:
	C4SliceResult c4foo_bar(C4Foo *foo, C4Slice input, C4Error *outError) noexcept {
	    try {
	        return C4SliceResult( foo->bar(input) );
	    } catch (...) {
	        C4Error::fromCurrentException(outError);
	        return {};
	    }
	}

As you can see, this is equivalent to calling `foo->bar(input)`, with conversion from `alloc_slice` to `C4SliceResult` and catching any exceptions.

## Error/Exception Handling

### Determining The C4Error

To recover the LiteCore error within a `catch` block do this:
	    ...
	} catch (...) {
	    C4Error err = C4Error::fromCurrentException();

Or alternatively:
	    ...
	} catch (const std::exception &x) {
	    C4Error err = C4Error::fromException(x);

Note that this converts _any_ exception into a C4Error, even ones that have nothing to do with LiteCore. It tries to do its best on typical exceptions like `std::bad_alloc`, but in the general case it might resort to returning `kC4ErrorUnexpected`. So it’s best to detect and handle your own exception types, if any, before this.

### Not Every Failure Throws An Exception

There are some common situations where a method fails, but throwing an exception would be too awkward or expensive. In these cases the method will just return some kind of null value that indicates the failure. Some examples:
* `C4Database::getDocument()` returns `nullptr` if the document doesn’t exist.
* `C4BlobStore::getContents()` returns `nullslice` if there’s no blob with the given key.
* `C4Blob::keyFromDigestProperty` returns `nullopt` if the dictionary doesn’t contain a valid `digest` property.
In general, the API documentation in the header should indicate whether a method will return such a failure value.

## Building Your Binary

The biggest limitation of the C++ API is that **you must statically link LiteCore into your executable**. You can’t use the LiteCore dynamic library, because that only exports the functions in the C API, so the linker won’t find any of the C++ methods it’s looking for.

> (This is typical of C++ APIs. The language just doesn’t work well with dynamic linking, due the large number of symbols, compiler-specific name ‘mangling’, and the fact that it’s all too easy to break binary compatibility when classes have virtual methods.)

### FAQ
**Q: Can I mix and match APIs?**
**A: Yes!** You can totally use the two APIs together, as long as you accept some idiosyncrasies with header files. When including both C and C++ headers in the same source file, **include the C++ ones first** or you may get a compile error regarding the declaration of `C4Document`.

**Q: What’s the method that’s equivalent to `c4foo_bar()`?**
**A:** It’s _usually_ `C4Foo::bar()`. The method name might be slightly different, maybe `getBar`; you can skim `c4Foo.hh` to look for it. If you’re stuck, you can go find the implementation of `c4foo_bar` — usually in `c4CAPI.cc` — and look at what it calls. (In Xcode, Command-clicking on the function will take you to its declaration, and Command-clicking that will take you to the implementation.)

**Q: Do all the C++ methods throw exceptions?**
**A:** Not all. The ones that don’t are annotated with `noexcept` in the header. If you don’t see `noexcept` at the end of a method’s declaration, you should assume that it could throw an exception.
