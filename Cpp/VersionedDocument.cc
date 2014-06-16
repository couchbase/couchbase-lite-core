//
//  VersionedDocument.cc
//  CBForest
//
//  Created by Jens Alfke on 5/14/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#include "VersionedDocument.hh"
#include <assert.h>

namespace forestdb {

    VersionedDocument::VersionedDocument(Database* db, slice docID)
    :_db(db), _doc(docID)
    {
        _db->read(_doc);
        decode();
    }

    VersionedDocument::VersionedDocument(Database* db, const Document& doc)
    :_db(db), _doc(doc)
    {
        decode();
    }

    VersionedDocument::VersionedDocument(Database* db, Document&& doc)
    :_db(db), _doc(std::move(doc))
    {
        decode();
    }

    void VersionedDocument::decode() {
        if (_doc.body())
            RevTree::decode(_doc.body(), _doc.sequence(), _doc.offset());
        else
            _unknown = _doc.body().size > 0;        // i.e. doc was read as meta-only
    }

    revid VersionedDocument::revID() const {
        slice result = _doc.meta();
        if (result.size <= 1)
            return revid();
        result.moveStart(1);
        return revid(result);
    }

    VersionedDocument::Flags VersionedDocument::flags() const {
        slice meta = _doc.meta();
        if (meta.size < 1)
            return 0;
        return meta[0];
    }

    void VersionedDocument::updateMeta() {
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
    }

    bool VersionedDocument::isBodyOfNodeAvailable(const RevNode* node) const {
        if (node->body.buf)
            return true;
        if (node->oldBodyOffset == 0)
            return false;
        VersionedDocument oldVersDoc(_db, _db->getByOffset(node->oldBodyOffset, node->sequence));
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
        VersionedDocument oldVersDoc(_db, _db->getByOffset(node->oldBodyOffset, node->sequence));
        if (oldVersDoc.sequence() != node->sequence)
            return alloc_slice();
        node = oldVersDoc.get(node->revID);
        if (!node)
            return alloc_slice();
        return alloc_slice(node->body);
    }

    void VersionedDocument::save(Transaction& transaction) {
        if (!_changed)
            return;
        updateMeta();
        // Don't call _doc.setBody() because it'll invalidate all the pointers from RevNodes into
        // the existing body buffer.
        transaction.set(_doc.key(), _doc.meta(), encode());
        _changed = false;
    }

}