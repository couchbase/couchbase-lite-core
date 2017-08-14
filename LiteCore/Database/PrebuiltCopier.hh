//
//  PrebuiltCopier.hpp
//  LiteCore
//
//  Created by Jim Borden on 2017/08/15.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "FilePath.hh"
#include "Database.hh"
#include "c4Database.h"

namespace litecore {
    bool CopyPrebuiltDB(const FilePath& from, const FilePath& to, const C4DatabaseConfig*, C4Error*);
}
