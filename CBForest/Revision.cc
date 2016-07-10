//
//  Revision.cc
//  CBForest
//
//  Created by Jens Alfke on 7/8/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#include "Revision.hh"
#include "Error.hh"
#include "Fleece.hh"


// Separates the docID and the revID in the keys of non-current Revisions.
#define kDelimiter '\t'


namespace cbforest {


    Revision::Revision(Document&& doc)
    :_doc(std::move(doc))
    {
        readMeta();
    }


    Revision::Revision(slice docID,
                       const versionVector &vers,
                       slice body,
                       slice docType,
                       bool deleted,
                       bool hasAttachments,
                       bool current)
    {
        fleece::Encoder enc;
        enc.beginArray();
        enc << ((deleted ? kDeleted : 0) | (hasAttachments ? kHasAttachments : 0));
        enc << vers;
        enc << docType;
        enc.endArray();
        _doc.setMeta(enc.extractOutput());
        readMeta();

        setKey(docID, current);
        _doc.setBody(body);
    }


    Revision::Revision(slice docID, slice revID, KeyStore &keyStore, KeyStore::contentOptions opt) {
        setKey(docID, revID);
        if (keyStore.read(_doc, opt))
            readMeta();
    }


    void Revision::readMeta() {
        slice metaBytes = _doc.meta();
        if (metaBytes.size < 2)
            error::_throw(error::CorruptRevisionData);

        auto metaValue = fleece::Value::fromTrustedData(metaBytes);
        fleece::Array::iterator meta(metaValue->asArray());
        _flags = (Flags)meta.read()->asInt();
        _vers.readFrom(meta.read());
        _docType = meta.read()->asString();
        if (_docType.size == 0)
            _docType.buf = nullptr;
    }


    slice Revision::docID() const {
        slice d = _doc.key();
        auto delim = d.findByte(kDelimiter);
        if (delim)
            d = d.upTo(delim);
        return d;
    }

    bool Revision::isCurrent() const {
        return _doc.key().findByte(kDelimiter) == nullptr;
    }

    void Revision::setCurrent(bool current) {
        if (current != isCurrent())
            setKey(docID(), current);
    }

    void Revision::setKey(slice docid, bool current) {
        setKey(docid, current ? revID() : slice::null);
    }

    void Revision::setKey(slice docid, slice rev) {
        if (rev.size == 0) {
            _doc.setKey(docid);
        } else {
            char buf[docid.size + 1 + rev.size];
            slice dst(buf, sizeof(buf));
            dst.writeFrom(docid);
            dst.writeByte(kDelimiter);
            dst.writeFrom(rev);
            _doc.setKey(slice(buf, sizeof(buf)));
        }
    }

    alloc_slice Revision::startKeyForDocID(slice docID) {
        alloc_slice result(docID.size + 1);
        memcpy((void*)result.buf, docID.buf, docID.size);
        const_cast<uint8_t&>(result[docID.size]) = kDelimiter;
        return result;
    }

    alloc_slice Revision::endKeyForDocID(slice docID) {
        alloc_slice result = startKeyForDocID(docID);
        const_cast<uint8_t&>(result[docID.size])++;
        return result;
    }

}