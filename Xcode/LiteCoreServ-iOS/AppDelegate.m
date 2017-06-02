//
//  AppDelegate.m
//  LiteCoreServ-iOS
//
//  Created by Pasin Suriyentrakorn on 6/1/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#import "AppDelegate.h"
#import "LCSServer.h"

@interface AppDelegate ()

@end

@implementation AppDelegate

@synthesize window;

- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)launchOptions {
    [[LCSServer sharedInstance] start];
    return YES;
}


- (void)applicationWillTerminate:(UIApplication *)application {
    [[LCSServer sharedInstance] stop];
}


- (void)applicationWillResignActive:(UIApplication *)application {

}


- (void)applicationDidEnterBackground:(UIApplication *)application {
    
}


- (void)applicationWillEnterForeground:(UIApplication *)application {

}


- (void)applicationDidBecomeActive:(UIApplication *)application {
    
}

@end
