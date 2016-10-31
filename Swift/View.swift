//
//  View.swift
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


public typealias EmitFunction = (Val, Val?) -> ()
public typealias MapFunction = (Body, EmitFunction) throws -> ()


public final class View {

    public init(database: Database,
                path: String? = nil,
                name: String,
                create: Bool = true,
                readOnly: Bool = false,
                version: String,
                map: @escaping MapFunction) throws
    {
        var config = C4DatabaseConfig(create: create, readOnly: readOnly)
        var err = C4Error()
        self.database = database
        self.name = name
        self.map = map
        self.handle = c4view_open(database.dbHandle,
                                  C4Slice(path),
                                  C4Slice(name),
                                  C4Slice(version),
                                  &config,
                                  &err)
        guard self.handle != nil else {
            throw err
        }
    }

    deinit {
        guard c4view_close(handle, nil) else {
            print("WARNING: \(self) is busy, couldn't be closed")
            return
        }
    }

    public let database: Database
    public let name: String

    public func eraseIndex() throws {
        var err = C4Error()
        guard c4view_eraseIndex(handle, &err) else {
            throw err
        }
    }

    public func delete() throws {
        var err = C4Error()
        guard c4view_delete(handle, &err) else {
            throw err
        }
        handle = nil
    }

    public class func delete(_ path: String) throws {
        var config = C4DatabaseConfig()
        var err = C4Error()
        guard c4view_deleteAtPath(C4Slice(path), &config, &err) else {
            throw err
        }
    }

    public var totalRows: UInt64                {return c4view_getTotalRows(handle)}
    public var lastSequenceIndexed: Sequence    {return c4view_getLastSequenceIndexed(handle)}
    public var lastSequenceChangedAt: Sequence  {return c4view_getLastSequenceChangedAt(handle)}


    public var map: MapFunction


    var handle: OpaquePointer?
}


extension View : CustomStringConvertible {
    public var description: String {
        return "View{\(name)}"
    }
}




public final class ViewIndexer {

    public init(views: [View]) throws {
        self.views = views
        var c4views = views.map { $0.handle }
        var err = C4Error()
        indexer = c4indexer_begin(views[0].database.dbHandle, &c4views, c4views.count, &err)
        guard indexer != nil else {
            throw err
        }
    }

    deinit {
        if indexer != nil {
            c4indexer_end(indexer, false, nil)  // run must have failed, or was never called
        }
    }

    public var database: Database {
        return views[0].database
    }

    public func run() throws {
        // Define the emit function that will be passed to the map function:
        var keys = [Key]()
        var values = [Data?]()
        func emit(_ keyObj: Val, valueObj: Val?) {
            print("    emit(\(keyObj), \(valueObj))")
            keys.append(Key(keyObj))
            values.append(valueObj?.asJSON())
        }

        // Enumerate the new revisions:
        var err = C4Error()
        let e = try enumerator()
        while let doc = e.next() {
            guard let body = try doc.selectedRevBody() else {
                continue
            }
            let props = try Val(json: body).asDict()!

            var viewNumber: UInt32 = 0
            for view in views {
                keys.removeAll(keepingCapacity: true)
                values.removeAll(keepingCapacity: true)

                // Call the map function!
                do {
                    try view.map(props, emit)
                } catch let x {
                    print("WARNING: View map function failed with error \(x)")
                }

                // Send the emitted keys/values to the indexer:
                if keys.count > 0 {
                    guard c4indexer_emit(indexer, doc.doc, viewNumber,
                                         UInt32(keys.count),
                                         keys.map {$0.handle},
                                         values.map {C4Slice($0)},
                                         &err)
                    else {
                        throw err
                    }
                }
                viewNumber += 1
            }
        }

        let ok = c4indexer_end(indexer, true, &err)
        indexer = nil
        guard ok else { throw err }
    }

    fileprivate func enumerator() throws -> DocEnumerator {
        var err = C4Error()
        let result = c4indexer_enumerateDocuments(indexer, &err)
        guard result != nil else {
            throw err
        }
        return DocEnumerator(database: database, c4enum: result)
    }

    fileprivate let views: [View]
    fileprivate var indexer: OpaquePointer?
}
