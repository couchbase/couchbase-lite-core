//
//  DocID.hh
//  LiteCore
//
//  Created by Jens Alfke on 6/19/19.
//  Copyright © 2019 Couchbase. All rights reserved.
//

#pragma once
#include "Base.hh"

namespace litecore {

    /** A database document/record ID. Type-safe wrapper around an alloc_slice.
        Invisibly padded with a null byte so it can be accessed as a C string. */
    class DocID {
    public:
        DocID()                                 { }

        explicit DocID(fleece::slice str)
        :_contents(alloc_slice::nullPaddedString(str))
        { }

        template <typename T>
        explicit DocID(T param)                 :DocID(slice(param)) {}

        DocID(const DocID &id)                  :_contents(id._contents) { }
        DocID(DocID &&id) noexcept              :_contents(std::move(id._contents)) { }

        DocID& operator=(const DocID &id)       {_contents = id._contents; return *this;}
        DocID& operator=(DocID &&id) noexcept   {_contents = std::move(id._contents); return *this;}

        const fleece::alloc_slice& asSlice() const      {return _contents;}

        const char *c_str() const                       {return (const char*)_contents.buf;}

        size_t size() const                             {return _contents.size;}

        explicit operator fleece::slice() const         {return _contents;}
        explicit operator fleece::alloc_slice() const   {return _contents;}

        explicit operator std::string() const           {return std::string(_contents);}

        bool operator== (const DocID &id) const         {return _contents == id._contents;}
        bool operator!= (const DocID &id) const         {return _contents != id._contents;}

    private:
        fleece::alloc_slice _contents;
    };


    /** A database sequence number. */
    typedef uint64_t sequence_t;

}
