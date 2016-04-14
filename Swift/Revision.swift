//
//  Revision.swift
//  SwiftForest
//
//  Created by Jens Alfke on 11/5/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//

import Foundation


public class Revision {

    init(database: Database, docID: String, revID: String, flags: C4RevisionFlags, rawBody: NSData?) {
        self.database = database
        self.docID = docID
        self.revID = revID
        self.flags = flags
        self.rawBody = rawBody
    }

    convenience init(database: Database, doc: Document, rawBody: NSData?) {
        self.init(database: database, docID: doc.docID, revID: doc.selectedRevID!, flags: doc.selectedRevFlags, rawBody: rawBody)
    }

    public let database: Database
    public let docID: String
    public let revID: String
    public let rawBody: NSData?

    private let flags: C4RevisionFlags

    public var deleted: Bool        {return flags.contains(C4RevisionFlags.RevDeleted)}
    public var hasAttachments: Bool {return flags.contains(C4RevisionFlags.RevHasAttachments)}

    public var generation: UInt {
        guard let dash = revID.rangeOfString("-") else {
            return 0
        }
        return UInt(revID.substringToIndex(dash.startIndex)) ?? 0
    }

    func update(body: JSONDict, deleted: Bool = false) throws -> Revision {
        let rawBody = try NSJSONSerialization.dataWithJSONObject(body, options: [])
        let doc = try database.putDoc(docID, parentRev: revID, body: rawBody, deletion: deleted)
        return Revision(database: database, doc: doc, rawBody: rawBody)
    }

    func delete() throws -> Revision {
        return try update([:], deleted: true)
    }

    // MARK:- PROPERTY ACCESS

    public lazy var properties: JSONDict = {
        do {
            if let json = self.rawBody,
                let props = try NSJSONSerialization.JSONObjectWithData(json, options: []) as? JSONDict {
                return props
            }
        } catch { }
        return [:]
    }()

    func property<T>(name: String) -> T? {
        return properties[name] as? T
    }

    func property<T>(name: String, default: T) -> T {
        return (properties[name] as? T) ?? `default`
    }

}


extension Revision : CustomStringConvertible {
    public var description: String {
        return "{\"\(docID)\" \(revID)}"
    }
}
