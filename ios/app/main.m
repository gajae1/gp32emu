/*
 * gp32emu iOS entry point.
 *
 * The app is scene based: the application delegate hands each connecting scene
 * a configuration naming GP32SceneDelegate, and that delegate owns the window
 * and the one frontend view controller. There is exactly one emulator owner
 * queue in the process, inside GP32Emulator, and the UI never calls the core
 * itself.
 */

#import <UIKit/UIKit.h>

#import "GP32EmulatorViewController.h"

@interface GP32SceneDelegate : UIResponder <UIWindowSceneDelegate>
@property (nonatomic, strong, nullable) UIWindow *window;
@end

@interface GP32AppDelegate : UIResponder <UIApplicationDelegate>
@end

@implementation GP32SceneDelegate

- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session
      options:(UISceneConnectionOptions *)connectionOptions {
    (void)session;
    (void)connectionOptions;
    if (![scene isKindOfClass:[UIWindowScene class]]) return;

    UIWindow *window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    window.rootViewController = [[GP32EmulatorViewController alloc] initWithNibName:nil bundle:nil];
    window.backgroundColor = [UIColor blackColor];
    [window makeKeyAndVisible];
    self.window = window;
}

@end

@implementation GP32AppDelegate

- (BOOL)application:(UIApplication *)application
        didFinishLaunchingWithOptions:(NSDictionary<UIApplicationLaunchOptionsKey, id> *)launchOptions {
    (void)application;
    (void)launchOptions;
    return YES;
}

- (UISceneConfiguration *)application:(UIApplication *)application
    configurationForConnectingSceneSession:(UISceneSession *)connectingSceneSession
                                   options:(UISceneConnectionOptions *)options {
    (void)application;
    (void)connectingSceneSession;
    (void)options;
    UISceneConfiguration *configuration =
        [[UISceneConfiguration alloc] initWithName:@"GP32emu"
                                      sessionRole:connectingSceneSession.role];
    configuration.delegateClass = GP32SceneDelegate.class;
    return configuration;
}

@end

int main(int argc, char *argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass(GP32AppDelegate.class));
    }
}
