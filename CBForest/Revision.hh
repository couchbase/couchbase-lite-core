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
#include "revid.hh"
#include "VersionVector.hh"


namespace cbforest {

    class Revision {
    public:

        typedef std::unique_ptr<Revision> Ref;

        struct BodyParams {
            slice body;
            slice docType;
            bool deleted;
            bool hasAttachments;
        };

        //typedef uint8_t Flags;
        enum Flags : uint8_t {
            kNone           = 0x00,
            kDeleted        = 0x01,
            kConflicted     = 0x02,
            kHasAttachments = 0x04,
        };

        /** Creates a Revision from a pre-populated Document read from a Database. */
        Revision(Document&& doc);

        /** Creates a new Revision. */
        Revision(slice docID,
                 const VersionVector &vers,
                 BodyParams,
                 bool current);

        Revision(Revision&&);

        slice docID() const;

        const VersionVector& version() const    {return _vers;}
        alloc_slice revID() const               {return _vers.current().asString();}
        bool isFromCASServer() const            {return _vers.isFromCASServer();}
        generation CAS() const                  {return _cas;}

        Flags flags() const                 {return _flags;}
        bool isDeleted() const              {return (flags() & kDeleted) != 0;}
        bool isConflicted() const           {return (flags() & kConflicted) != 0;}
        bool hasAttachments() const         {return (flags() & kHasAttachments) != 0;}

        bool exists() const                 {return _doc.exists();}
        cbforest::sequence sequence() const {return _doc.sequence();}

        slice docType() const               {return _docType;}
        slice body() const                  {return _doc.body();}

        const Document& document() const    {return _doc;}
        Document& document()                {return _doc;}

        bool isCurrent() const;
        void setCurrent(bool current);

    private:
        void readMeta();
        void setKey(slice docid, bool current);

        Document _doc;
        Flags _flags {kNone};
        VersionVector _vers;
        generation _cas {0};
        slice _docType;
    };

}


#endif /* Revision_hh */
