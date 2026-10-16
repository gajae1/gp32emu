/*
 * GP32EmulatorViewController: the whole UIKit frontend.
 *
 * Layout contract: the 4:3 guest picture has its own fixed view and never
 * contains a control. Every control sits in the top or bottom bar outside the
 * picture, so a touch that aims at a game button can never reach the menu. The
 * menu is one separate control whose only action is TouchUpInside; no other
 * touch stream, gesture or long press is wired to it.
 *
 * Threading: this class only ever touches GP32Emulator from the main thread.
 * The emulator marshals every core call onto its single owner queue, and the
 * frame handler it calls back is a main-thread block carrying an immutable
 * pixel snapshot.
 */

#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

@interface GP32EmulatorViewController : UIViewController
@end

NS_ASSUME_NONNULL_END
