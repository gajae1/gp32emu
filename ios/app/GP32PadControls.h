/*
 * GP32PadControls: the two input controls of the UIKit frontend.
 *
 * Both are plain UIControls. A press is reported only for a touch that began
 * inside the control, and every end and cancel path clears it, so a finger that
 * never lifts cannot leave a guest button stuck. Nothing here knows about the
 * menu: the menu is a separate control in the view controller with its own
 * action, so no game touch can ever activate it.
 *
 * All methods run on the main thread.
 */

#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

/* One guest button. */
@interface GP32PadButton : UIControl

- (instancetype)initWithBit:(uint32_t)bit caption:(NSString *)caption NS_DESIGNATED_INITIALIZER;
- (instancetype)init NS_UNAVAILABLE;
- (instancetype)initWithFrame:(CGRect)frame NS_UNAVAILABLE;

@property (nonatomic, readonly) uint32_t bit;
@property (nonatomic, readonly) UILabel *captionLabel;
@property (nonatomic, copy, nullable) void (^pressHandler)(void);
@property (nonatomic, copy, nullable) void (^releaseHandler)(void);

/* Clears the pressed state without reporting another press; used when the
 * session pauses or the menu opens while a finger is still down. */
- (void)cancelPress;

@end

/* The d-pad is one control so a finger can slide between directions without
 * lifting; the mask it reports is recomputed from the current contact point. */
@interface GP32PadView : UIControl

@property (nonatomic, copy, nullable) void (^maskHandler)(uint32_t mask); /* GP32_BUTTON_{UP,DOWN,LEFT,RIGHT} bits */
- (void)resetMask;

@end

NS_ASSUME_NONNULL_END
