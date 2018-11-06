//
// Tokenizer.hh
//
// Copyright © 2018 Couchbase. All rights reserved.
//

#pragma once
#include "c4Tokenizer.h"

struct sqlite3;

namespace litecore {

    /** The name under which the tokenizer is registered with SQLite. */
    constexpr const char* kC4TokenizerName = "C4Tokenizer";

    /** Registers the given factory function. Can only be called once. */
    void RegisterC4TokenizerFactory(C4TokenizerFactory factory);

    /** Returns true if a tokenizer factory has been registered. */
    bool HaveC4Tokenizer();

    /** Registers the tokenizer with a SQLite connection. */
    int InstallC4Tokenizer(struct sqlite3 *db);
}
