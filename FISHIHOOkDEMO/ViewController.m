//
//  ViewController.m
//  FISHIHOOkDEMO
//
//  Created by chenzheng on 2020/5/19.
//  Copyright © 2020 CST. All rights reserved.
//

#import "ViewController.h"
#import "fishhook.h"

@interface ViewController ()

@end

@implementation ViewController

- (void)viewDidLoad
{
    [super viewDidLoad];
    NSLog(@"123");
    struct rebinding nslog = {
        .name = "NSLog",
        .replacement = my_NSLog,
        // 系统的 NSlog 的指针被放到了这里
        .replaced = (void *) &sys_nslog
    };
    struct rebinding rebs[1] = {nslog}; // 数组的目的：可以一次进行多个 hook
    rebind_symbols(rebs, 1);
    NSLog(@"end");
}

// 函数指针变量
static void (*sys_nslog)(NSString *format, ...);

void my_NSLog(NSString *format, ...)
{
    format = [format stringByAppendingString:@"✅"];
    sys_nslog(format);
}

@end
