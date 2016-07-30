//
//  KeyStore.hh
//  CBNano
//
//  Created by Jens Alfke on 11/12/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#ifndef __CBNano__KeyStore__
#define __CBNano__KeyStore__

#include "Base.hh"
#include "DocEnumerator.hh"
#include <functional>

namespace cbforest {

    using namespace std;

    class Database;
    class Document;
    class Transaction;

    /** A sequence number in a KeyStore. */
    typedef uint64_t sequence;


    /** A container of key/value mappings. Keys and values are opaque blobs.
        The value is divided into 'meta' and 'body'; the body can optionally be omitted when
        reading, to save time/space. There is also a 'sequence' number that's assigned every time
        a value is saved, from an incrementing counter.
        A key, meta and body together are called a Document.
        This is an abstract class; the Database instance acts as its factory and will instantiate
        the appropriate subclass for the storage engine in use. */
    class KeyStore {
    public:

        struct Options {
            bool sequences      :1;     //< Documents have sequences & can be enumerated by sequence
            bool softDeletes    :1;     //< Deleted documents have sequence numbers (until compact)

            static const Options defaults;
        };


        Database& database() const                  {return _db;}
        const string& name() const                  {return _name;}
        Options options() const                     {return _options;}

        virtual uint64_t documentCount() const =0;
        virtual sequence lastSequence() const =0;

        // Keys/values:

        Document get(slice key, ContentOptions = kDefaultContent) const;
        virtual Document get(sequence, ContentOptions = kDefaultContent) const =0;

        virtual void get(slice key, ContentOptions, function<void(const Document&)>);
        virtual void get(sequence, ContentOptions, function<void(const Document&)>);

        /** Reads a document whose key() is already set. */
        virtual bool read(Document &doc, ContentOptions options = kDefaultContent) const =0;

        /** Reads the body of a Document that's already been read with kMetaonly. */
        virtual void readBody(Document &doc) const      {read(doc, kDefaultContent);}

        virtual Document getByOffsetNoErrors(uint64_t offset, sequence) const
                {return Document();}

        //////// Writing:

        virtual sequence set(slice key, slice meta, slice value, Transaction&) =0;
        sequence set(slice key, slice value, Transaction &t)
                                                        {return set(key, slice::null, value, t);}
        void write(Document&, Transaction&);

        bool del(slice key, Transaction&);
        bool del(sequence s, Transaction&);
        bool del(Document&, Transaction&);

        virtual void erase() =0;

        void deleteKeyStore(Transaction&);

        // public for complicated reasons; clients should never call it
        virtual ~KeyStore()                             { }

    protected:
        KeyStore(Database &db, const string &name, Options options)
                :_db(db), _name(name), _options(options) { }

        virtual void reopen()                           { }
        virtual void close()                            { }

        virtual bool _del(slice key, Transaction&) =0;
        virtual bool _del(sequence s, Transaction&) =0;

        virtual DocEnumerator::Impl* newEnumeratorImpl(slice minKey, slice maxKey,
                                                       DocEnumerator::Options&) =0;
        virtual DocEnumerator::Impl* newEnumeratorImpl(sequence min, sequence max,
                                                       DocEnumerator::Options&) =0;

        void updateDoc(Document &doc, sequence seq, uint64_t offset =0, bool exists =true) const {
            doc.update(seq, offset, exists);
        }

        Database &_db;
        const string _name;
        const Options _options;

    private:
        KeyStore(const KeyStore&) = delete;
        KeyStore& operator=(const KeyStore&) = delete;

        friend class Database;
        friend class DocEnumerator;
        friend class KeyStoreWriter;
    };

}

#endif /* defined(__CBNano__KeyStore__) */
