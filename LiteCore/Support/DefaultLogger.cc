//
//  DefaultLogger.cc
//  LiteCore
//
//  Created by Jim Borden on 2017/04/28.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "Logging.hh"

extern "C" {
    litecore::LogDomain DefaultLog("", litecore::LogLevel::Info);
}
