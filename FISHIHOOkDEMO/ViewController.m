//
//  ViewController.m
//  FISHIHOOkDEMO
//
//  Created by chenzheng on 2020/5/19.
//  Copyright © 2020 CST. All rights reserved.
//

#import "ViewController.h"
#import "fishhook.h"
#import <dlfcn.h>

@interface ViewController ()

@end

@implementation ViewController

- (void)viewDidLoad
{
    [super viewDidLoad];
    NSLog(@"123"); // 断点 1
    struct rebinding nslog = {
        .name = "NSLog",
        .replacement = my_NSLog,
        .replaced = (void *) &sys_nslog // 系统的 NSlog 的指针被放到了这里
    };
    struct rebinding rebs[1] = {nslog}; // 数组的目的：可以一次进行多个 hook
    rebind_symbols(rebs, 1);            // 断点 2
    NSLog(@"end");                      // 断点 3

    //    int (*my_ptracec)(int,pid_t,caddr_t,int);
    //
    //    void *handle = dlopen("usr/lib/system/libsystem_kernel.dylib", RTLD_LAZY); // ptrace() 就在 libsystem_kernel.dyliblimian
    //    my_ptracec = dlsym(handle, "ptrace"); // 返回函数指针，告别了 PIC 技术，脱离了符号表，阻止了 fishhook 的 hook
    //    my_ptracec(PT_DENY_ATTACH,0,0);

    /**
     经过 my_ptracec ，在下一个符号断点"ptrace",会被断主，那如何找到调用的地方呢？
     攻击：使用 hoper / ida，找到字符串常量"ptrace"，改掉对应的二进制常量，相当于 ptraee，无法调用，达到破解的目的
     防护：字符串加密，异或去做
     
     */
}

// 函数指针变量
static void (*sys_nslog)(NSString *format, ...);

//FOUNDATION_EXPORT void NSLog(NSString *format, ...) NS_FORMAT_FUNCTION(1,2) NS_NO_TAIL_CALL;
void my_NSLog(NSString *format, ...)
{
    format = [format stringByAppendingString:@"✅"];
    sys_nslog(format);
}

- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{
    [super touchesBegan:touches withEvent:event];
    NSLog(@"屏幕点击");
}

@end
