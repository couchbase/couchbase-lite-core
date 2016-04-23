//  Copyright © 2015 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

package com.couchbase.cbforest;

public class ForestException extends Exception {
    public final int domain; // TODO: Should be an enum
    public final int code;

    public ForestException(int domain, int code, String message) {
        super(message);
        this.domain = domain;
        this.code = code;
    }

    public static void throwException(int domain, int code, String msg) throws ForestException {
        throw new ForestException(domain, code, msg);
    }
}
