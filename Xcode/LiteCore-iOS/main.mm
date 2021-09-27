//
//  main.mm
//  LiteCore Shell
//
//  Created by Jens Alfke on 9/27/16.
//  Copyright 2016-Present Couchbase, Inc.
//
//  Use of this software is governed by the Business Source License included
//  in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
//  in that file, in accordance with the Business Source License, use of this
//  software will be governed by the Apache License, Version 2.0, included in
//  the file licenses/APL2.txt.
//

#import <UIKit/UIKit.h>
#import "AppDelegate.h"

#define CATCH_CONFIG_CONSOLE_WIDTH 120
#define CATCH_CONFIG_RUNNER
#include "catch.hpp"
#include "QuietReporter.hh"

int main(int argc, char * argv[]) {
#if 0
    Catch::Session session;
    session.configData().reporterNames.push_back("list");
    session.configData().useColour = Catch::UseColour::No; // otherwise it tries to use ANSI escapes in Xcode console

    session.applyCommandLine(1, argv);
    session.run();
#else
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([AppDelegate class]));
    }
#endif
}
