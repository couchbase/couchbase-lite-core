//
// DocumentFactory.hh
//
// Copyright © 2021 Couchbase. All rights reserved.
//

#pragma once
#include "c4Base.hh"
#include "c4DocumentTypes.h"
#include "Record.hh"
#include <vector>

C4_ASSUME_NONNULL_BEGIN

namespace litecore {

    /** Abstract interface for creating Document instances; owned by a Database. */
    class DocumentFactory : protected C4Base {
    public:
        using ContentOption = litecore::ContentOption;
        using Record = litecore::Record;

        DocumentFactory(C4Collection *coll)                     :_coll(coll) { }
        virtual ~DocumentFactory() =default;
        C4Collection* collection() const                        {return _coll;}

        virtual bool isFirstGenRevID(slice revID) const         {return false;}

        virtual Retained<C4Document> newDocumentInstance(slice docID, ContentOption) =0;
        virtual Retained<C4Document> newDocumentInstance(const Record&) =0;

        virtual std::vector<alloc_slice> findAncestors(const std::vector<slice> &docIDs,
                                                       const std::vector<slice> &revIDs,
                                                       unsigned maxAncestors,
                                                       bool mustHaveBodies,
                                                       C4RemoteID remoteDBID) =0;

    private:
        C4Collection* const _coll;    // Unretained, to avoid ref-cycle
    };

}

C4_ASSUME_NONNULL_END
