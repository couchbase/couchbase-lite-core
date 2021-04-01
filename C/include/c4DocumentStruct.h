//
// c4DocumentStruct.h
//
// Copyright © 2021 Couchbase. All rights reserved.
//

#pragma once
#include "c4DocumentTypes.h"

#ifndef LITECORE_CPP_API
#define LITECORE_CPP_API 0
#endif

C4_ASSUME_NONNULL_BEGIN
C4API_BEGIN_DECLS


/** Describes a version-controlled document. */
struct
#if ! LITECORE_CPP_API                        // C++ has a different declaration, in c4Document.hh
C4Document
#else
C4Document_C
#endif
{
    void* _internal1, *_internal2;              // placeholders for vtable-ptr and refcount in C++
    C4DocumentFlags flags;      ///< Document flags
    C4HeapString docID;         ///< Document ID
    C4HeapString revID;         ///< Revision ID of current revision
    C4SequenceNumber sequence;  ///< Sequence at which doc was last updated

    C4Revision selectedRev;     ///< Describes the currently-selected revision

    C4ExtraInfo extraInfo;      ///< For client use
};


C4API_END_DECLS
C4_ASSUME_NONNULL_END
