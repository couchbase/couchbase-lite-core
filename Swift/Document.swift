//
//  Document.swift
//  SwiftForest
//
//  Created by Jens Alfke on 11/4/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//

import Foundation


public class Document {

    init(handle: UnsafeMutablePointer<C4Document>) {
        doc = handle
    }

    deinit {
        c4doc_free(doc)
    }

    public var deleted: Bool        {return doc.memory.flags.contains(C4DocumentFlags.Deleted)}
    public var conflicted: Bool     {return doc.memory.flags.contains(C4DocumentFlags.Conflicted)}
    public var hasAttachments: Bool {
        return doc.memory.flags.contains(C4DocumentFlags.HasAttachments)}
    public var exists: Bool         {return doc.memory.flags.contains(C4DocumentFlags.Exists)}

    public var docID: String        {return doc.memory.docID.asString()!}
    public var revID: String        {return doc.memory.revID.asString() ?? ""}
    public var sequence: Sequence   {return doc.memory.sequence}

    public var docType: String? {
        get {
            let t = c4doc_getType(doc)
            let result = t.asString()
            c4slice_free(t)
            return result
        }
        set {
            c4doc_setType(doc, C4Slice(newValue))
        }
    }

    // Selected revision:

    public var selectedRevID: String?            {return doc.memory.selectedRev.revID.asString()}
    public var selectedRevFlags: C4RevisionFlags {return doc.memory.selectedRev.flags}
    public var selectedRevSequence: Sequence     {return doc.memory.selectedRev.sequence}
    public var selectedRevHasBody: Bool          {return c4doc_hasRevisionBody(doc)}

    public func selectedRevBody() throws -> NSData? {
        var body = doc.memory.selectedRev.body
        if body.buf == nil {
            var err = C4Error()
            guard c4doc_loadRevisionBody(doc, &err) else {
                throw err
            }
            body = doc.memory.selectedRev.body
        }
        return body.asData()
    }

    // Selecting revisions:

    public func selectRevision(revID: String, withBody: Bool = true) throws -> Bool {
        var err = C4Error()
        guard c4doc_selectRevision(doc, C4Slice(revID), withBody,  &err) else {
            if err.code == 404 {
                return false
            }
            throw err
        }
        return true
    }

    public func selectCurrentRevision() -> Bool {return c4doc_selectCurrentRevision(doc)}
    public func selectParentRevision() -> Bool  {return c4doc_selectParentRevision(doc)}
    public func selectNextRevision() -> Bool    {return c4doc_selectNextRevision(doc)}

    public func selectNextLeafRevision(includeDeleted: Bool = false, withBody: Bool = true) throws -> Bool {
        var err = C4Error()
        guard c4doc_selectNextLeafRevision(doc, includeDeleted, withBody, &err) else {
            if err.code == 404 {
                return false
            }
            throw err
        }
        return true
    }

    // Adding revisions (low-level):

    public func addRevision(revID: String, body: NSData, deleted: Bool = false,
                            hasAttachments: Bool = false, allowConflict: Bool = false) throws -> Bool
    {
        var err = C4Error()
        let added = c4doc_insertRevision(doc, C4Slice(revID), C4Slice(body), deleted, hasAttachments, allowConflict, &err)
        guard added >= 0 else {
            throw err
        }
        return added > 0
    }

    public func insertRevision(body: NSData, deleted: Bool = false,
                               hasAttachments: Bool = false, history: [String]) throws -> Int
    {
        var historySlices = history.map {C4Slice($0)}
        var err = C4Error()
        let added = c4doc_insertRevisionWithHistory(doc, C4Slice(body), deleted, hasAttachments,
            &historySlices, history.count, &err)
        guard added >= 0 else {
            throw err
        }
        return Int(added)
    }

    public func purgeRevision(revID: String) throws -> Int {
        var err = C4Error()
        let purged = c4doc_purgeRevision(doc, C4Slice(revID), &err)
        guard purged >= 0 else {
            throw err
        }
        return Int(purged)
    }

    public func save(maxRevTreeDepth: UInt32 = 20) throws {
        var err = C4Error()
        guard c4doc_save(doc, maxRevTreeDepth, &err) else {
            throw err
        }
    }


    var doc: UnsafeMutablePointer<C4Document>
}


extension Document : CustomStringConvertible {
    public var description: String {
        return "{\"\(docID)\"}"
    }
}
