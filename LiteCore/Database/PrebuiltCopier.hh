//
//  PrebuiltCopier.hpp
//  LiteCore
//
//  Created by Jim Borden on 2017/08/15.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once

struct C4DatabaseConfig;

namespace litecore {
    class FilePath;
    
    void CopyPrebuiltDB(const FilePath& from, const FilePath& to, const C4DatabaseConfig*);
}
