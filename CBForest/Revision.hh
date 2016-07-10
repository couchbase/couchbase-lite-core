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
        typedef uint8_t Flags;
        enum {
            kDeleted        = 0x01,
            kConflicted     = 0x02,
            kHasAttachments = 0x04,
        };

        Revision(Document&& doc);

        Revision(slice docID,
                 const versionVector &vers,
                 slice body,
                 slice docType,
                 bool deleted,
                 bool hasAttachments,
                 bool current);

        Revision(slice docID, slice revID, KeyStore&,
                 KeyStore::contentOptions = KeyStore::kDefaultContent);

        slice docID() const;
        const versionVector& version() const    {return _vers;}
        alloc_slice revID() const               {return _vers.current().asString();}
        bool isFromCASServer() const            {return _vers.isFromCASServer();}

        Flags flags() const                 {return _flags;}
        bool isDeleted() const              {return (flags() & kDeleted) != 0;}
        bool isConflicted() const           {return (flags() & kConflicted) != 0;}
        bool hasAttachments() const         {return (flags() & kHasAttachments) != 0;}

        bool exists() const                 {return _doc.exists();}
        cbforest::sequence sequence() const {return _doc.sequence();}

        slice docType() const               {return _docType;}

        const Document& document() const    {return _doc;}
        Document& document()                {return _doc;}

        bool isCurrent() const;
        void setCurrent(bool current);

        static alloc_slice startKeyForDocID(slice docID);
        static alloc_slice endKeyForDocID(slice docID);

    private:
        void readMeta();
        void setKey(slice docid, bool current);
        void setKey(slice docid, slice revid);

        Document _doc;
        Flags _flags {0};
        versionVector _vers;
        slice _docType;
    };

}


#endif /* Revision_hh */
