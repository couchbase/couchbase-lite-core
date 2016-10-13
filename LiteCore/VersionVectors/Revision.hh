//
//  Revision.hh
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 7/8/16.
//  Copyright (c) 2016 Couchbase. All rights reserved.
//

#pragma once
#include "Record.hh"
#include "VersionVector.hh"


namespace litecore {

    /** A single revision of a versioned document, stored as an individual record. */
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

        /** Flags applying to the record if this is the current rev. Matches C4DocumentFlags. */
        enum Flags : uint8_t {
            kNone           = 0x00,
            kDeleted        = 0x01,
            kConflicted     = 0x02,
            kHasAttachments = 0x04,
        };

        /** Creates a Revision from a pre-populated Record read from a DataFile. */
        explicit Revision(const Record& rec);

        /** Creates a new Revision. */
        Revision(slice docID,
                 const VersionVector &vers,
                 BodyParams,
                 bool current);

        Revision(Revision&&) noexcept;

        slice docID() const;
        alloc_slice revID() const           {return _vers ? _vers.current().asString()
                                                          : alloc_slice();}
        const VersionVector& version() const{return _vers;}

        Flags flags() const                 {return _flags;}
        bool isDeleted() const              {return (flags() & kDeleted) != 0;}
        bool isConflicted() const           {return (flags() & kConflicted) != 0;}
        bool hasAttachments() const         {return (flags() & kHasAttachments) != 0;}

        bool exists() const                 {return _rec.exists();}
        sequence_t sequence() const         {return _rec.sequence();}

        slice docType() const               {return _recType;}
        slice body() const                  {return _rec.body();}

        Record& record()                    {return _rec;}

        bool isCurrent() const;
        void setCurrent(bool);

        bool setConflicted(bool);

    private:
        void readMeta();
        void writeMeta(const VersionVector &vers);
        void setKey(slice docid, bool current);

        Revision(const Revision&) =delete;  // not copyable
        Revision& operator= (const Revision&) =delete;

        Record          _rec;               // The record
        Flags           _flags {kNone};     // Flags
        VersionVector   _vers;              // Version vector
        slice           _recType;           // Record type
    };

}

