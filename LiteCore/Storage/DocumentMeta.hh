//
//  DocumentMeta.hh
//  LiteCore
//
//  Created by Jens Alfke on 12/8/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#pragma once
#include "Base.hh"
#include "Record.hh"


namespace litecore {

    /** Flags applying to the document (and its current revision). Matches C4DocumentFlags. */
    enum DocumentFlags : uint8_t {
        kNone           = 0x00,
        kDeleted        = 0x01,
        kConflicted     = 0x02,
        kHasAttachments = 0x04,
    };


    /** Document metadata. Encodes itself to store in a Record's meta(). */
    struct DocumentMeta {
        DocumentMeta()                      :flags() { }
        DocumentMeta(DocumentFlags, slice version);
        DocumentMeta(slice meta);
        DocumentMeta(const Record &rec)     :DocumentMeta(rec.meta()) { }

        void setFlag(DocumentFlags f)       {flags = (DocumentFlags)(flags | f);}
        void clearFlag(DocumentFlags f)     {flags = (DocumentFlags)(flags & ~f);}

        void decode(slice data);
        alloc_slice encode() const;
        alloc_slice encodeAndUpdate();

        DocumentFlags flags;
        slice version;
    };
}
