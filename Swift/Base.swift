//
//  Base.swift
//  SwiftForest
//
//  Created by Jens Alfke on 11/5/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//

import Foundation


/** A document sequence number. Every time a document is updated, it's assigned the next
    available sequence number. */
public typealias Sequence = UInt64


public typealias JSONDict = [String:AnyObject]


extension C4Error: ErrorType {
}

extension C4Error : CustomStringConvertible {
    public var description: String {
        let domainNames = ["HTTP", "errno", "ForestDB", "C4"]
        return "C4Error(\(domainNames[Int(domain.rawValue)]) \(code))"
    }
}


extension C4Slice {
    public init(_ s: String?) {
        if let str = s {
            let data = str.dataUsingEncoding(NSUTF8StringEncoding)
            buf = data!.bytes
            size = data!.length
        } else {
            buf = nil
            size = 0
        }
    }

    public init(_ s: NSString?) {
        if let str = s {
            let data = str.dataUsingEncoding(NSUTF8StringEncoding)
            buf = data!.bytes
            size = data!.length
        } else {
            buf = nil
            size = 0
        }
    }

    public init(_ data: NSData?) {
        if let d = data {
            buf = d.bytes
            size = d.length
        } else {
            buf = nil
            size = 0
        }
    }

    public func asData() -> NSData? {
        guard buf != nil else {
            return nil
        }
        return NSData(bytes: UnsafeMutablePointer(buf), length: size)
    }

    // Fast but destructive conversion to NSData. Afterwards the slice will be empty.
    public mutating func toData() -> NSData? {
        guard buf != nil else {
            return nil
        }
        let data = NSData(bytesNoCopy: UnsafeMutablePointer(buf), length: size)
        buf = nil
        size = 0
        return data
    }

    public func asNSString() -> NSString? {
        guard buf != nil else {
            return nil
        }
        return NSString(bytes: buf, length: size, encoding: NSUTF8StringEncoding)
    }

    public func asString() -> String? {
        return self.asNSString() as? String //OPT: Do this w/o going through NSString
    }
}
