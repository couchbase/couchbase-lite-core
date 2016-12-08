//
//  Revision.cc
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 7/8/16.
//  Copyright (c) 2016 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#include "Revision.hh"
#include "RevisionStore.hh"
#include "Error.hh"
#include "Fleece.hh"


namespace litecore {


    Revision::Revision(const Record& rec)
    :_rec(rec)
    {
        if (_rec.meta().buf || _rec.exists())
            readMeta();
    }


    Revision::Revision(slice docID,
                       const VersionVector &vers,
                       BodyParams p,
                       bool current)
    {
        // Create metadata:
        if (p.deleted)
            _meta.flags = kDeleted;
        if (p.hasAttachments)
            _meta.setFlag(kHasAttachments);
        _meta.docType = p.docType;

        writeMeta(vers);

        // Set the rec key and body:
        setKey(docID, current);
        _rec.setBody(p.body);
    }


    Revision::Revision(Revision &&old) noexcept
    :_rec(std::move(old._rec)),
     _meta(old._meta),
     _vers(std::move(old._vers))
    { }


    void Revision::writeMeta(const VersionVector &vers) {
        std::string versStr = vers.asString();
        _meta.version = slice(versStr);
        _rec.setMeta(_meta.encode());
        // Read it back in, to set up my pointers into it:
        readMeta();
    }


    void Revision::readMeta() {
        _meta.decode(_rec.meta());
        _vers = VersionVector(_meta.version);
    }


    bool Revision::setConflicted(bool conflicted) {
        if (conflicted == isConflicted())
            return false;
        if (conflicted)
            _meta.setFlag(kConflicted);
        else
            _meta.clearFlag(kConflicted);
        writeMeta(_vers);
        return true;
    }


#pragma mark DOC ID / KEYS:


    slice Revision::docID() const {
        return RevisionStore::docIDFromKey(_rec.key());
    }

    bool Revision::isCurrent() const {
        return docID().size == _rec.key().size;
    }

    void Revision::setCurrent(bool current) {
        if (current != isCurrent())
            setKey(docID(), current);
    }

    void Revision::setKey(slice docid, bool current) {
        if (current)
            _rec.setKey(docid);
        else
            _rec.setKey(RevisionStore::keyForNonCurrentRevision(docid, _vers.current()));
    }

}
