//
//  Revision.swift
//  SwiftForest
//
//  Created by Jens Alfke on 11/5/15.
//  Copyright (c) 2015-2016 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

/********
 NOTE: THIS IS A PROVISIONAL, PLACEHOLDER API, NOT THE OFFICIAL COUCHBASE LITE 2.0 API.
 It's for prototyping, experimentation, and performance testing. It will change without notice.
 Once the 2.0 API is designed, we will begin implementing that and remove these classes.
 ********/


import Foundation


public class Revision : PropertyOwner {

    init?(doc: Document, rev :C4Revision) {
        guard let revID = rev.revID.asString() else {
            return nil
        }
        self.document = doc
        self.revID = revID
        self.flags = rev.flags
        self.rawBody = rev.body.asData()
    }

    public let document: Document
    public let revID: String
    public let rawBody: Data?

    let flags: C4RevisionFlags

    public var deleted: Bool        {return flags.contains(C4RevisionFlags.revDeleted)}
    public var hasAttachments: Bool {return flags.contains(C4RevisionFlags.revHasAttachments)}

    public func put(_ body: Body, deleted: Bool = false) throws -> Revision {
        let rawBody: Data = try Val.withObject(body).asJSON()
        try document.putDoc(parentRev: revID, body: rawBody, deletion: deleted)
        let newRev = document.selectedRevision()!
        document.selectCurrentRevision()
        return newRev
    }

    func delete() throws -> Revision {
        return try put([:], deleted: true)
    }

    // MARK:- PROPERTY ACCESS

    public lazy var properties: Body = {
        do {
            return try self.getProperties()
        } catch {
            return [:]
        }
    }()

}


extension Revision : CustomStringConvertible {
    public var description: String {
        return "{\"\(document.docID)\" \(revID)}"
    }
}


// Document methods for Revisions
extension Document {

    public func currentRevision() -> Revision? {
        // The c4Document is always left with its current revision selected.
        return selectedRevision()
    }

    public func revisionWithID(_ revID: String, withBody: Bool = true) throws -> Revision? {
        var err = C4Error()
        guard c4doc_selectRevision(doc, C4Slice(revID), withBody,  &err) else {
            if err.code == 404 {
                return nil
            }
            throw err
        }
        defer {selectCurrentRevision()}
        return selectedRevision()
    }

    public func leafRevisions(includeDeleted: Bool = false, withBodies: Bool = true) throws -> [Revision] {
        var revs = [Revision]()
        var err = C4Error()
        defer {selectCurrentRevision()}
        repeat {
            guard let rev = selectedRevision() else {break}
            revs.append(rev)
        } while c4doc_selectNextLeafRevision(doc, includeDeleted, withBodies, &err)
        if err.code != 0 {
            throw err
        }
        return revs
    }

    
    // Internals:

    func selectedRevision() -> Revision? {
        return Revision(doc: self, rev: doc.pointee.selectedRev)
    }

    func selectCurrentRevision() {
        c4doc_selectCurrentRevision(doc)
    }

}
