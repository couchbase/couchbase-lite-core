//
//  MapReduceIndex.hh
//  CBForest
//
//  Created by Jens Alfke on 5/15/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#ifndef __CBForest__MapReduceIndex__
#define __CBForest__MapReduceIndex__

#include "Index.hh"
#include <vector>


namespace forestdb {

    /** A document as passed to a map function. This is subclassable; subclasses can transform
        the document (e.g. parsing JSON) and provide additional methods to access the transformed
        version. (Look at MapReduce_Test.mm for an example.) */
    class Mappable {
    public:
        explicit Mappable(const Document& doc)      :_doc(doc) { }
        virtual ~Mappable()                         { }

        const Document& document() const            {return _doc;}

    private:
        const Document& _doc;
    };

    class EmitFn {
    public:
        virtual void operator() (Collatable key, Collatable value) =0;
    };

    class MapFn {
    public:
        virtual void operator() (const Mappable&, EmitFn& emit) =0;
    };

    /** An Index that uses a MapFn to index the documents of another Database. */
    class MapReduceIndex : public Index {
    public:
        MapReduceIndex(std::string path,
                       Database::openFlags,
                       const Database::config&,
                       Database* sourceDatabase);

        Database* sourceDatabase() const        {return _sourceDatabase;}
        void readState();
        int indexType() const                   {return _indexType;}
        
        void setup(int indexType, MapFn *map, std::string mapVersion);

        /** The last source database sequence number to be indexed. */
        sequence lastSequenceIndexed() const;

        /** The last source database sequence number at which the index actually changed. (<= lastSequenceIndexed.) */
        sequence lastSequenceChangedAt() const;

    private:
        void invalidate();
        void saveState(IndexTransaction& t);
        bool updateDocInIndex(IndexTransaction&, const Mappable&);

        forestdb::Database* _sourceDatabase;
        MapFn* _map;
        std::string _mapVersion, _lastMapVersion;
        int _indexType;
        sequence _lastSequenceIndexed, _lastSequenceChangedAt;
        sequence _stateReadAt; // index sequence # at which state was last valid

        friend class MapReduceIndexer;
    };


    /** An activity that updates one or more map-reduce indexes. */
    class MapReduceIndexer {
    public:
        MapReduceIndexer(std::vector<MapReduceIndex*> indexes);
        ~MapReduceIndexer();

        bool run();

    protected:
        /** Transforms the Document to a Mappable and invokes addMappable.
            The default implementation just uses the Mappable base class, i.e. doesn't do any work.
            Subclasses can override this to do arbitrary parsing or transformation of the doc.
            The override probably shouldn't call the inherited method, just addMappable(). */
        virtual void addDocument(const Document&);

        /** Calls each index's map function on the Mappable, and updates the indexes. */
        void addMappable(const Mappable&);

    private:
        std::vector<MapReduceIndex*> _indexes;
        std::vector<IndexTransaction*> _transactions;
    };
}

#endif /* defined(__CBForest__MapReduceIndex__) */
