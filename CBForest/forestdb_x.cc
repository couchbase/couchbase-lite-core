//
//  forestdb_x.c
//  CBForest
//
//  Created by Jens Alfke on 3/29/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#include "forestdb_x.h"
#include "internal_types.h"
#include <stdlib.h>


extern
uint64_t _docio_read_doc_component(struct docio_handle *handle,
                                   uint64_t offset,
                                   uint32_t len,
                                   void *buf_out);

#ifdef _DOC_COMP
extern
uint64_t _docio_read_doc_component_comp(struct docio_handle *handle,
                                        uint64_t offset,
                                        uint32_t len,
                                        uint32_t comp_len,
                                        void *buf_out,
                                        void *comp_data_out);
#endif


//FIX: Remove this function when MB-10695 is implemented.
fdb_status x_fdb_read_body(fdb_handle *db, fdb_doc *doc, uint64_t _offset) {
    docio_handle *handle = db->dhandle;
    doc->body = (void *)malloc(doc->bodylen);
#ifdef _DOC_COMP
    // If compression is enabled, I can't tell from looking at just the body whether it's
    // compressed. So the offset is useless. As a workaround, read the doc the normal way:
    free(doc->body);
    doc->body = NULL;
    return fdb_get(db, doc);
#else
    _offset = _docio_read_doc_component(handle, _offset, (uint32_t)doc->bodylen, doc->body);
    if (_offset == 0) {
        return FDB_RESULT_KEY_NOT_FOUND;
    }
#endif
    return FDB_RESULT_SUCCESS;
}
