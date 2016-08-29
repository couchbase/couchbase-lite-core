//
//  Document.swift
//  SwiftForest
//
//  Created by Jens Alfke on 11/4/15.
//  Copyright (c) 2015-2016 Couchbase. All rights reserved.
//

import Foundation


public typealias Body = [String:Val]


open class Document {

    init(handle: UnsafeMutablePointer<C4Document>) {
        doc = handle
    }

    deinit {
        c4doc_free(doc)
    }

    open var deleted: Bool        {return doc.pointee.flags.contains(C4DocumentFlags.deleted)}
    open var conflicted: Bool     {return doc.pointee.flags.contains(C4DocumentFlags.conflicted)}
    open var hasAttachments: Bool {
        return doc.pointee.flags.contains(C4DocumentFlags.hasAttachments)}
    open var exists: Bool         {return doc.pointee.flags.contains(C4DocumentFlags.exists)}

    open var docID: String        {return doc.pointee.docID.asString()!}
    open var revID: String        {return doc.pointee.revID.asString() ?? ""}
    open var sequence: Sequence   {return doc.pointee.sequence}

    open var docType: String? {
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

    open var selectedRevID: String?            {return doc.pointee.selectedRev.revID.asString()}
    open var selectedRevFlags: C4RevisionFlags {return doc.pointee.selectedRev.flags}
    open var selectedRevSequence: Sequence     {return doc.pointee.selectedRev.sequence}
    open var selectedRevHasBody: Bool          {return c4doc_hasRevisionBody(doc)}

    open func selectedRevBody() throws -> Data? {
        var body = doc.pointee.selectedRev.body
        if body.buf == nil {
            var err = C4Error()
            guard c4doc_loadRevisionBody(doc, &err) else {
                throw err
            }
            body = doc.pointee.selectedRev.body
        }
        return body.asData()
    }

    // Selecting revisions:

    open func selectRevision(_ revID: String, withBody: Bool = true) throws -> Bool {
        var err = C4Error()
        guard c4doc_selectRevision(doc, C4Slice(revID), withBody,  &err) else {
            if err.code == 404 {
                return false
            }
            throw err
        }
        return true
    }

    open func selectCurrentRevision() -> Bool {return c4doc_selectCurrentRevision(doc)}
    open func selectParentRevision() -> Bool  {return c4doc_selectParentRevision(doc)}
    open func selectNextRevision() -> Bool    {return c4doc_selectNextRevision(doc)}

    open func selectNextLeafRevision(_ includeDeleted: Bool = false, withBody: Bool = true) throws -> Bool {
        var err = C4Error()
        guard c4doc_selectNextLeafRevision(doc, includeDeleted, withBody, &err) else {
            if err.code == 404 {
                return false
            }
            throw err
        }
        return true
    }

    open func purgeRevision(_ revID: String) throws -> Int {
        var err = C4Error()
        let purged = c4doc_purgeRevision(doc, C4Slice(revID), &err)
        guard purged >= 0 else {
            throw err
        }
        return Int(purged)
    }

    open func save(_ maxRevTreeDepth: UInt32 = 20) throws {
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
