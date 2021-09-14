//
//  ViewController.m
//  LiteCoreServ-iOS
//
//  Created by Pasin Suriyentrakorn on 6/1/17.
//  Copyright 2017-Present Couchbase, Inc.
//
//  Use of this software is governed by the Business Source License included
//  in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
//  in that file, in accordance with the Business Source License, use of this
//  software will be governed by the Apache License, Version 2.0, included in
//  the file licenses/APL2.txt.
//

#import "ViewController.h"
#import "LCSServer.h"
#import "LCSServerConfig.h"


@interface ViewController () <LCSServerDelegate> {
    __weak IBOutlet UITextView *textView;
}

@end

@implementation ViewController

- (void)viewDidLoad {
    [super viewDidLoad];
    
    LCSServer* serv = [LCSServer sharedInstance];
    serv.delegate = self;
    
    [self updateStatus];
}


#pragma mark - LCSServerDelegate


- (void) didStartListenerWithError:(NSError *)error {
    [self updateStatus];
}


- (void) didStopListener {
    [self updateStatus];
}


#pragma mark - Private

- (void) updateStatus {
    LCSServer* serv = [LCSServer sharedInstance];
    LCSServerConfig* config = serv.config;
    
    NSString* admin = [NSString stringWithFormat: @"Admin: %lu", (unsigned long) config.adminPort];
    NSString* listener;
    if (serv.isListenerRunning)
        listener = [NSString stringWithFormat: @"Listener: %lu", (unsigned long) config.port];
    else
        listener = @"Listener: Stopped";
    NSString* error = serv.error ? [NSString stringWithFormat:@"Error: %@", serv.error] : @"";
    
    textView.text = [NSString stringWithFormat:@"%@ | %@\n%@", admin, listener, error];
}


@end
