//
//  forestdb_x.h
//  CBForest
//
//  Created by Jens Alfke on 3/29/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#ifndef CBForest_forestdb_x_h
#define CBForest_forestdb_x_h

// Stuff that should be in the ForestDB API

#include "forestdb_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Read the body of a document given its offset (as obtained from fdb_get_metaonly or
 * fdb_get_metaonly_byseq.)
 * The doc's key and meta are ignored; only the body_offset is used to locate the body.
 * The bodylen field must already be correctly set to the body length.
 * (This unofficial function is a workaround until MB-10695 is implemented.)
 * @param handle Pointer to ForestDB handle.
 * @param doc Pointer to ForestDB doc instance whose doc body is populated as a result
 *        of this API call.
 * @return FDB_RESULT_SUCCESS on success.
 */
fdb_status x_fdb_read_body(fdb_handle *db, fdb_doc *doc, uint64_t body_offset);

#ifdef __cplusplus
}
#endif

#endif
