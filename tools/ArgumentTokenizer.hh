//
//  ArgumentTokenizer.hh
//  LiteCore
//
//  Created by Jim Borden on 2018/01/06.
//  Copyright © 2018 Couchbase. All rights reserved.
//

#pragma once
#include <string>
#include <deque>

class ArgumentTokenizer {
public:
    bool tokenize(const char* input, std::deque<std::string> &args);
};

