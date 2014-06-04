//
//  VersionedDocument.cc
//  CBForest
//
//  Created by Jens Alfke on 5/14/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#include "VersionedDocument.hh"

namespace forestdb {

    VersionedDocument::VersionedDocument(Database* db, slice docID)
    :_db(db), _doc(docID)
    {
        _db->read(_doc);
        if (_doc.body())
            decode(_doc.body(), _doc.sequence(), _doc.offset());
    }

    VersionedDocument::VersionedDocument(Database* db, Document&& doc)
    :_db(db), _doc(doc)
    {
        decode(_doc.body(), _doc.sequence(), _doc.offset());
    }

    slice VersionedDocument::revID() const {
        slice result = _doc.meta();
        if (result.size <= 1)
            return slice::null;
        result.moveStart(1);
        return result;
    }

    VersionedDocument::Flags VersionedDocument::flags() const {
        slice meta = _doc.meta();
        if (meta.size < 1)
            return 0;
        return meta[0];
    }

    bool VersionedDocument::isBodyOfNodeAvailable(const RevNode* node) const {
        if (node->body.buf)
            return true;
        if (node->oldBodyOffset == 0)
            return false;
        VersionedDocument oldVersDoc(_db, _db->getByOffset(node->oldBodyOffset));
        if (oldVersDoc.sequence() != node->sequence)
            return false;
        node = oldVersDoc.get(node->revID);
        return (node && node->body.buf);
    }

    alloc_slice VersionedDocument::readBodyOfNode(const RevNode* node) const {
        if (node->body.buf)
            return alloc_slice(node->body);
        if (node->oldBodyOffset == 0)
            return alloc_slice();
        VersionedDocument oldVersDoc(_db, _db->getByOffset(node->oldBodyOffset));
        if (oldVersDoc.sequence() != node->sequence)
            return alloc_slice();
        node = oldVersDoc.get(node->revID);
        if (!node)
            return alloc_slice();
        return alloc_slice(node->body);
    }

    Document& VersionedDocument::document() {
        if (_changed) {
            const RevNode* curNode = currentNode();
            slice revID = curNode->revID;
            Flags flags = 0;
            if (curNode->isDeleted())
                flags |= kDeleted;
            if (hasConflict())
                flags |= kConflicted;

            alloc_slice newMeta(1+revID.size);
            (uint8_t&)newMeta[1] = flags;
            memcpy((void*)&newMeta[1], revID.buf, revID.size);
            _doc.setMeta(newMeta);
            _doc.setBody(encode());
            _changed = false;
        }
        return _doc;
    }

    void VersionedDocument::save(Transaction& transaction) {
        transaction.write(document());
    }

}