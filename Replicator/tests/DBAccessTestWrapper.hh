//
// DBAccessTestWrapper.hh
//
// Copyright (C) 2020 Jens Alfke. All Rights Reserved.
//

#pragma once
#include "c4DocEnumerator.h"
#include <memory>

struct DBAccessTestWrapper {

    static C4DocEnumerator* unresolvedDocsEnumerator(C4Database*);

    static unsigned numDeltasApplied();
};
