//
//  main.mm
//  LiteCore Shell
//
//  Created by Jens Alfke on 9/27/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#import <UIKit/UIKit.h>
#import "AppDelegate.h"

#define CATCH_CONFIG_CONSOLE_WIDTH 120
#define CATCH_CONFIG_RUNNER
#include "catch.hpp"
#include "CaseListReporter.hh"

int main(int argc, char * argv[]) {
    Catch::Session session;
    session.configData().reporterNames.push_back("list");
    session.configData().useColour = Catch::UseColour::No; // otherwise it tries to use ANSI escapes in Xcode console

    session.applyCommandLine(argc, argv);
    session.run();

//    @autoreleasepool {
//        return UIApplicationMain(argc, argv, nil, NSStringFromClass([AppDelegate class]));
//    }
}
