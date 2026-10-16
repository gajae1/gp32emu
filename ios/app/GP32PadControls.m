#import "GP32PadControls.h"

#include "gp32emu/gp32.h"

@implementation GP32PadButton {
    BOOL _pressed;
    BOOL _trackingCancelled;
}

- (instancetype)initWithBit:(uint32_t)bit caption:(NSString *)caption {
    self = [super initWithFrame:CGRectZero];
    if (!self) return nil;
    _bit = bit;
    self.backgroundColor = [UIColor colorWithRed:0.10 green:0.14 blue:0.19 alpha:1.0];
    self.layer.cornerRadius = 9.0;
    self.layer.borderWidth = 1.0;
    self.layer.borderColor = [[UIColor colorWithWhite:0.7 alpha:1.0] CGColor];
    self.multipleTouchEnabled = NO;
    self.accessibilityLabel = caption;
    self.isAccessibilityElement = YES;
    self.accessibilityTraits = UIAccessibilityTraitButton;

    UILabel *label = [[UILabel alloc] initWithFrame:CGRectZero];
    label.text = caption;
    label.textColor = [UIColor colorWithWhite:0.95 alpha:1.0];
    label.font = [UIFont systemFontOfSize:15.0 weight:UIFontWeightSemibold];
    label.textAlignment = NSTextAlignmentCenter;
    label.userInteractionEnabled = NO;
    label.translatesAutoresizingMaskIntoConstraints = NO;
    [self addSubview:label];
    [NSLayoutConstraint activateConstraints:@[
        [label.centerXAnchor constraintEqualToAnchor:self.centerXAnchor],
        [label.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
    ]];
    _captionLabel = label;
    return self;
}

- (void)setPressed:(BOOL)pressed {
    if (_pressed == pressed) return;
    _pressed = pressed;
    self.backgroundColor = pressed ? [UIColor colorWithRed:0.18 green:0.53 blue:0.80 alpha:1.0]
                                    : [UIColor colorWithRed:0.10 green:0.14 blue:0.19 alpha:1.0];
    if (pressed) {
        if (self.pressHandler) self.pressHandler();
    } else if (self.releaseHandler) {
        self.releaseHandler();
    }
}

- (void)cancelPress {
    [self setPressed:NO];
}

- (BOOL)beginTrackingWithTouch:(UITouch *)touch withEvent:(UIEvent *)event {
    (void)touch;
    (void)event;
    _trackingCancelled = NO;
    [self setPressed:YES];
    return YES;
}

- (void)endTrackingWithTouch:(UITouch *)touch withEvent:(UIEvent *)event {
    (void)touch;
    (void)event;
    [self setPressed:NO];
}

- (BOOL)continueTrackingWithTouch:(UITouch *)touch withEvent:(UIEvent *)event {
    (void)event;
    if (_trackingCancelled) return NO;
    [self setPressed:CGRectContainsPoint(self.bounds, [touch locationInView:self])];
    return YES;
}

- (void)cancelTrackingWithEvent:(UIEvent *)event {
    (void)event;
    _trackingCancelled = YES;
    [self setPressed:NO];
}

@end

@implementation GP32PadView {
    uint32_t _mask;
    BOOL _trackingCancelled;
}

- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (!self) return nil;
    self.backgroundColor = UIColor.clearColor;
    self.opaque = NO;
    self.contentMode = UIViewContentModeRedraw;
    self.multipleTouchEnabled = NO;
    self.accessibilityLabel = @"D-pad";
    self.isAccessibilityElement = YES;
    return self;
}

- (void)setMask:(uint32_t)mask {
    if (_mask == mask) return;
    _mask = mask;
    [self setNeedsDisplay];
    if (self.maskHandler) self.maskHandler(mask);
}

- (void)resetMask {
    [self setMask:0u];
}

- (void)updateMaskForPoint:(CGPoint)point {
    if (!CGRectContainsPoint(self.bounds, point)) { [self setMask:0u]; return; }
    CGPoint center = CGPointMake(CGRectGetMidX(self.bounds), CGRectGetMidY(self.bounds));
    CGFloat dx = point.x - center.x;
    CGFloat dy = point.y - center.y;
    /* Android uses a 3 by 3 grid with a neutral centre cell. */
    CGFloat dead = CGRectGetWidth(self.bounds) / 6.0;
    uint32_t mask = 0;
    if (dx < -dead) mask |= GP32_BUTTON_LEFT;
    if (dx >= dead) mask |= GP32_BUTTON_RIGHT;
    if (dy < -dead) mask |= GP32_BUTTON_UP;
    if (dy >= dead) mask |= GP32_BUTTON_DOWN;
    [self setMask:mask];
}

- (void)drawRect:(CGRect)rect {
    (void)rect;
    CGFloat cell = CGRectGetWidth(self.bounds) / 3;
    CGFloat h = CGRectGetHeight(self.bounds) / 3;
    /* Keep the square hit region for diagonal sliding, but draw a real cross.
     * Highlight both arms for a diagonal, not a single generic pressed state. */
    CGRect arms[] = {CGRectMake(cell, 0, cell, h), CGRectMake(cell, 2*h, cell, h),
                     CGRectMake(0, h, cell, h), CGRectMake(2*cell, h, cell, h)};
    uint32_t bits[] = {GP32_BUTTON_UP, GP32_BUTTON_DOWN, GP32_BUTTON_LEFT, GP32_BUTTON_RIGHT};
    UIColor *idle = [UIColor colorWithRed:0.10 green:0.14 blue:0.19 alpha:1];
    UIColor *active = [UIColor colorWithRed:0.18 green:0.53 blue:0.80 alpha:1];
    [idle setFill];
    UIRectFill(CGRectMake(cell - 1, h - 1, cell + 2, h + 2));
    for (NSUInteger i = 0; i < 4; ++i) {
        UIBezierPath *arm = [UIBezierPath bezierPathWithRoundedRect:CGRectInset(arms[i], 1, 1) cornerRadius:7];
        [(_mask & bits[i] ? active : idle) setFill];
        [arm fill];
        [[UIColor colorWithWhite:0.7 alpha:0.6] setStroke];
        arm.lineWidth = 1;
        [arm stroke];
        CGPoint c = CGPointMake(CGRectGetMidX(arms[i]), CGRectGetMidY(arms[i]));
        CGFloat t = MIN(cell, h) * 0.12;
        UIBezierPath *arrow = [UIBezierPath bezierPath];
        if (i < 2) {
            CGFloat sign = i == 0 ? -1 : 1;
            [arrow moveToPoint:CGPointMake(c.x, c.y + sign*t)];
            [arrow addLineToPoint:CGPointMake(c.x - t, c.y - sign*t)];
            [arrow addLineToPoint:CGPointMake(c.x + t, c.y - sign*t)];
        } else {
            CGFloat sign = i == 2 ? -1 : 1;
            [arrow moveToPoint:CGPointMake(c.x + sign*t, c.y)];
            [arrow addLineToPoint:CGPointMake(c.x - sign*t, c.y - t)];
            [arrow addLineToPoint:CGPointMake(c.x - sign*t, c.y + t)];
        }
        [arrow closePath];
        [UIColor.whiteColor setFill];
        [arrow fill];
    }
}

- (BOOL)beginTrackingWithTouch:(UITouch *)touch withEvent:(UIEvent *)event {
    (void)event;
    _trackingCancelled = NO;
    [self updateMaskForPoint:[touch locationInView:self]];
    return YES;
}

- (BOOL)continueTrackingWithTouch:(UITouch *)touch withEvent:(UIEvent *)event {
    (void)event;
    if (_trackingCancelled) return NO;
    [self updateMaskForPoint:[touch locationInView:self]];
    return YES;
}

- (void)endTrackingWithTouch:(UITouch *)touch withEvent:(UIEvent *)event {
    (void)touch;
    (void)event;
    [self resetMask];
}

- (void)cancelTrackingWithEvent:(UIEvent *)event {
    (void)event;
    _trackingCancelled = YES;
    [self resetMask];
}

@end
