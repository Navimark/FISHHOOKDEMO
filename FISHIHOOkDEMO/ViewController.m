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

- (void)viewDidLoad {
    [super viewDidLoad];
    struct rebinding rebs[1] = {0};
    rebind_symbols(rebs, 1);
}


- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{
    [super touchesBegan:touches withEvent:event];
}

@end
