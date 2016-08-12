//
//  Revision.hh
//  CBForest
//
//  Created by Jens Alfke on 7/8/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#ifndef Revision_hh
#define Revision_hh
#include "Document.hh"
#include "VersionVector.hh"


namespace cbforest {

    /** A revision of a versioned document, which is (confusingly) stored as a separate CBForest
        document. */
    class Revision {
    public:

        typedef std::unique_ptr<Revision> Ref;

        struct BodyParams {
            slice body;
            slice docType;
            bool deleted;
            bool hasAttachments;
            bool conflicted;
        };

        /** Flags applying to the document if this is the current rev. Matches C4DocumentFlags. */
        enum Flags : uint8_t {
            kNone           = 0x00,
            kDeleted        = 0x01,
            kConflicted     = 0x02,
            kHasAttachments = 0x04,
        };

        /** Creates a Revision from a pre-populated Document read from a Database. */
        explicit Revision(const Document& doc);

        /** Creates a new Revision. */
        Revision(slice docID,
                 const VersionVector &vers,
                 BodyParams,
                 bool current);

        Revision(Revision&&);

        slice docID() const;
        alloc_slice revID() const           {return _vers ? _vers.current().asString()
                                                          : alloc_slice();}
        const VersionVector& version() const{return _vers;}

        Flags flags() const                 {return _flags;}
        bool isDeleted() const              {return (flags() & kDeleted) != 0;}
        bool isConflicted() const           {return (flags() & kConflicted) != 0;}
        bool hasAttachments() const         {return (flags() & kHasAttachments) != 0;}

        bool exists() const                 {return _doc.exists();}
        sequence_t sequence() const         {return _doc.sequence();}

        slice docType() const               {return _docType;}
        slice body() const                  {return _doc.body();}

        Document& document()                {return _doc;}

        bool isCurrent() const;
        void setCurrent(bool);

        bool setConflicted(bool);

    private:
        void readMeta();
        void writeMeta(const VersionVector &vers);
        void setKey(slice docid, bool current);

        Revision(const Revision&) =delete;  // not copyable
        Revision& operator= (const Revision&) =delete;

        Document        _doc;               // The document
        Flags           _flags {kNone};     // Flags
        VersionVector   _vers;              // Version vector
        slice           _docType;           // Document type
    };

}


#endif /* Revision_hh */
