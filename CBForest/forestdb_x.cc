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


fdb_status x_fdb_read_body(fdb_handle *db, fdb_doc *doc, uint64_t offset) {
    void *body = malloc(doc->bodylen);
    uint64_t _offset;
#ifdef _DOC_COMP
    _offset = _docio_read_doc_component_comp(db->dhandle, offset, &doc->bodylen, body);
#else
    _offset = _docio_read_doc_component(db->dhandle, offset, (uint32_t)doc->bodylen, body);
#endif
    if (_offset == 0) {
        free(body);
        return FDB_RESULT_FAIL;
    }
    doc->body = body;
    return FDB_RESULT_SUCCESS;
}
