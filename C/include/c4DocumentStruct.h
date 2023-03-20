//
// c4DocumentStruct.h
//
// Copyright 2021-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "c4DocumentTypes.h"

#ifndef LITECORE_CPP_API
#    define LITECORE_CPP_API 0
#endif

C4_ASSUME_NONNULL_BEGIN
C4API_BEGIN_DECLS

// Ignore warning about not initializing members, it must be this way to be C-compatible
// NOLINTBEGIN(cppcoreguidelines-pro-type-member-init)
/** Describes a version-controlled document. */
struct
#if !LITECORE_CPP_API  // C++ has a different declaration, in c4Document.hh
        C4Document
#else
        C4Document_C
#endif
{
    void* _internal1;  // placeholders for vtable-ptr and refcount (see c4Document.hh)
    void* _internal2;

    C4DocumentFlags  flags;     ///< Document flags
    C4HeapString     docID;     ///< Document ID
    C4HeapString     revID;     ///< Revision ID of current revision
    C4SequenceNumber sequence;  ///< Sequence at which doc was last updated

    C4Revision selectedRev;  ///< Describes the currently-selected revision

    C4ExtraInfo extraInfo;  ///< For client use
};

// NOLINTEND(cppcoreguidelines-pro-type-member-init)

C4API_END_DECLS
C4_ASSUME_NONNULL_END
