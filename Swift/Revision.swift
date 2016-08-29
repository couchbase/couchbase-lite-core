//
//  Revision.swift
//  SwiftForest
//
//  Created by Jens Alfke on 11/5/15.
//  Copyright (c) 2015-2016 Couchbase. All rights reserved.
//

import Foundation


open class Revision {

    init(database: Database, docID: String, revID: String, flags: C4RevisionFlags, rawBody: Data?) {
        self.database = database
        self.docID = docID
        self.revID = revID
        self.flags = flags
        self.rawBody = rawBody
    }

    convenience init(database: Database, doc: Document, rawBody: Data?) {
        self.init(database: database, docID: doc.docID, revID: doc.selectedRevID!, flags: doc.selectedRevFlags, rawBody: rawBody)
    }

    open let database: Database
    open let docID: String
    open let revID: String
    open let rawBody: Data?

    fileprivate let flags: C4RevisionFlags

    open var deleted: Bool        {return flags.contains(C4RevisionFlags.revDeleted)}
    open var hasAttachments: Bool {return flags.contains(C4RevisionFlags.revHasAttachments)}

    open var generation: UInt {
        guard let dash = revID.range(of: "-") else {
            return 0
        }
        return UInt(revID.substring(to: dash.lowerBound)) ?? 0
    }

    func update(_ body: Body, deleted: Bool = false) throws -> Revision {
        let rawBody: Data = try Val.withJSONObject(body).asJSON()
        let doc = try database.putDoc(docID, parentRev: revID, body: rawBody, deletion: deleted)
        return Revision(database: database, doc: doc, rawBody: rawBody)
    }

    func delete() throws -> Revision {
        return try update([:], deleted: true)
    }

    // MARK:- PROPERTY ACCESS

    open lazy var properties: Body = {
        do {
            if let json = self.rawBody, let d = try Val(json: json).asDict() {
                return d
            }
        } catch { }
        return [:]
    }()

    func property<T>(_ name: String) -> T? {
        return properties[name]?.unwrap() as? T
    }

    func property<T>(_ name: String, default: T) -> T {
        return (properties[name]?.unwrap() as? T) ?? `default`
    }

}


extension Revision : CustomStringConvertible {
    public var description: String {
        return "{\"\(docID)\" \(revID)}"
    }
}
