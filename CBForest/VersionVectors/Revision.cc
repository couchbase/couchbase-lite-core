//
//  Revision.cc
//  CBForest
//
//  Created by Jens Alfke on 7/8/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#include "Revision.hh"
#include "RevisionStore.hh"
#include "Error.hh"
#include "Fleece.hh"


namespace cbforest {


    Revision::Revision(const Document& doc)
    :_doc(doc)
    {
        if (_doc.meta().buf || _doc.exists())
            readMeta();
    }


    Revision::Revision(slice docID,
                       const VersionVector &vers,
                       BodyParams p,
                       bool current)
    {
        // Create metadata:
        if (p.deleted)
            _flags = kDeleted;
        if (p.hasAttachments)
            _flags = (Flags)(_flags | kHasAttachments);
        _docType = p.docType;

        writeMeta(vers);

        // Read it back in, to set up my pointers into it:
        readMeta();

        // Set the doc key and body:
        setKey(docID, current);
        _doc.setBody(p.body);
    }


    Revision::Revision(Revision &&old)
    :_doc(std::move(old._doc)),
     _flags(old._flags),
     _vers(std::move(old._vers)),
     _docType(std::move(old._docType))
    { }


    void Revision::writeMeta(const VersionVector &vers) {
        fleece::Encoder enc;
        enc.beginArray();
        enc << _flags;
        enc << vers;
        enc << _docType;
        enc.endArray();
        _doc.setMeta(enc.extractOutput());
    }


    void Revision::readMeta() {
        slice metaBytes = _doc.meta();
        if (metaBytes.size < 2)
            error::_throw(error::CorruptRevisionData);

        auto metaValue = fleece::Value::fromTrustedData(metaBytes);
        fleece::Array::iterator meta(metaValue->asArray());
        _flags = (Flags)meta.read()->asUnsigned();
        _vers.readFrom(meta.read());
        _docType = meta.read()->asString();
        if (_docType.size == 0)
            _docType.buf = nullptr;
    }


#pragma mark DOC ID / KEYS:


    slice Revision::docID() const {
        return RevisionStore::docIDFromKey(_doc.key());
    }

    bool Revision::isCurrent() const {
        return docID().size == _doc.key().size;
    }

    void Revision::setCurrent(bool current) {
        if (current != isCurrent())
            setKey(docID(), current);
    }

    void Revision::setKey(slice docid, bool current) {
        if (current)
            _doc.setKey(docid);
        else
            _doc.setKey(RevisionStore::keyForNonCurrentRevision(docid, _vers.current()));
    }

}
