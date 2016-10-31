//
//  Base.swift
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


/** A document sequence number. Every time a document is updated, it's assigned the next
    available sequence number. */
public typealias Sequence = UInt64


extension C4Error: Error {
}

extension C4Error : CustomStringConvertible {
    public var description: String {
        return c4error_getMessage(self).asString()!
    }
}

extension C4Error {
    public var isNotFound: Bool {
        return domain == .LiteCoreDomain && code == Int32(kC4ErrorNotFound)
    }
    public var isConflict: Bool {
        return domain == .LiteCoreDomain && code == Int32(kC4ErrorConflict)
    }
}


extension C4Slice {
    public init(_ s: String?) {
        if let str = s {
            let data = str.data(using: String.Encoding.utf8)
            buf = (data! as NSData).bytes
            size = data!.count
        } else {
            buf = nil
            size = 0
        }
    }

    public init(_ s: NSString?) {
        if let str = s {
            let data = str.data(using: String.Encoding.utf8.rawValue)
            buf = (data! as NSData).bytes
            size = data!.count
        } else {
            buf = nil
            size = 0
        }
    }

    public init(_ data: Data?) {
        if let d = data {
            buf = (d as NSData).bytes
            size = d.count
        } else {
            buf = nil
            size = 0
        }
    }

    public func asData() -> Data? {
        guard buf != nil else {
            return nil
        }
        return Data(bytes: buf, count: size)
    }

    // Fast but destructive conversion to NSData. Afterwards the slice will be empty.
    public mutating func toData() -> Data? {
        guard buf != nil else {
            return nil
        }
        let data = Data(bytesNoCopy: UnsafeMutableRawPointer(mutating:buf),
                        count: size,
                        deallocator: .free)
        buf = nil
        size = 0
        return data
    }

    public func asNSString() -> NSString? {
        guard buf != nil else {
            return nil
        }
        return NSString(bytes: buf, length: size, encoding: String.Encoding.utf8.rawValue)
    }

    public func asString() -> String? {
        return self.asNSString() as? String //OPT: Do this w/o going through NSString
    }
}
