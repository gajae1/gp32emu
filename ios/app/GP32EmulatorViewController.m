/*
 * GP32EmulatorViewController: the whole UIKit frontend.
 *
 * Layout contract: game controls sit below the native 4:3 picture in portrait
 * and beside it in landscape. The landscape menu overlays the upper center
 * briefly, then only its transparent hit area remains. Its only action is
 * TouchUpInside: no other touch stream activates it and no long press is used.
 *
 * Threading: this class touches GP32Emulator only from the main thread. The
 * emulator marshals every core call onto its single owner queue and calls the
 * frame handler back on the main thread with an immutable pixel snapshot.
 */

#import "GP32EmulatorViewController.h"

#import "GP32Audio.h"
#import "GP32Emulator.h"
#import "GP32PadControls.h"
#import "GP32JIT.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <GameController/GameController.h>
#import <QuartzCore/QuartzCore.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <CommonCrypto/CommonDigest.h>

#include <stdint.h>

/* The guest clock advances at 60 intervals/second, independently of display
 * callback jitter. At most two owed intervals are submitted per callback. */
static const NSTimeInterval GP32DisplayInterval = 1.0 / 60.0;
/* Absorbs callback jitter that lands a hair before the deadline. Because the
 * deadline advances by a full interval per tick, this cannot raise the
 * long-run guest rate above 60 intervals per second. */
static const NSTimeInterval GP32DisplayIntervalSlack = 1.0 / 300.0;

typedef NS_ENUM(NSInteger, GP32ImportKind) {
    GP32ImportGame = 0,
    GP32ImportBIOS,
};

static NSString *const GP32DefaultBIOSKey = @"GP32SelectedBIOS";
static NSString *const GP32DefaultGameKey = @"GP32SelectedGame";
static NSString *const GP32DefaultBootModeKey = @"GP32BootMode";

@interface GP32EmulatorViewController () <UIDocumentPickerDelegate, UIAdaptivePresentationControllerDelegate>
+ (NSString *)importURL:(NSURL *)source into:(NSString *)directory error:(NSError **)error;
/* Declared so a selector is visible wherever this class sends it to itself;
 * the definition order below is not part of the contract. */
- (void)buildInterface;
- (void)presentPixels:(NSData *)pixels;
- (void)setTouchMask:(uint32_t)touchMask;
- (void)setKeyboardMask:(uint32_t)keyboardMask;
- (void)setControllerMask:(uint32_t)controllerMask;
- (void)pushButtonMask;
- (void)clearInputState;
- (void)installKeyboardInput;
- (void)stopKeyboardInput;
- (void)handleKeyCode:(GCKeyCode)code pressed:(BOOL)pressed;
- (void)installControllerInput;
- (void)stopControllerInput;
- (void)refreshControllers;
- (uint32_t)controllerBitsForPad:(GCExtendedGamepad *)pad intoMask:(uint32_t)mask;
- (void)handleController:(GCExtendedGamepad *)pad;
- (void)installDisplayLink;
- (void)displayTick:(CADisplayLink *)link;
- (BOOL)shouldRun;
- (void)applyRunningState;
- (void)restoreSelectionIfPossible;
- (void)loadContentAtPath:(NSString *)gamePath;
- (void)chooseBootMode:(gp32_ios_boot_mode_t)mode;
- (void)presentPickerForImport:(GP32ImportKind)kind;
- (void)presentSoon:(UIViewController *)controller;
- (void)showMenu:(id)sender;
- (void)revealMenuButton;
- (void)showStatus:(NSString *)message temporary:(BOOL)temporary;
- (void)menuDidClose;
- (void)pauseForBackground;
- (void)handleAudioInterruption:(NSNotification *)note;
- (void)handleAudioSystemNotification:(NSNotification *)note;
- (void)applicationDidBecomeActive:(NSNotification *)note;
- (void)applicationWillResignActive:(NSNotification *)note;
- (void)applicationDidEnterBackground:(NSNotification *)note;
#if GP32EMU_IOS_DEVELOPER_JIT
- (void)requestExternalJIT;
- (void)pollExternalJIT;
- (void)finishExternalJIT:(NSString *)message;
#endif
@end

@implementation GP32EmulatorViewController {
    GP32Audio *_audio;
    GP32Emulator *_emulator;

    UIView *_pictureView;
    UILabel *_statusLabel;
    NSTimer *_statusHideTimer;
    GP32PadView *_pad;
    NSMutableArray<GP32PadButton *> *_padButtons;
    GP32PadButton *_leftShoulder, *_rightShoulder, *_startButton, *_selectButton;
    GP32PadButton *_buttonA, *_buttonB;
    UIButton *_menuButton;
    UIView *_menuGlyph;
    NSTimer *_menuHideTimer;
    BOOL _landscapeLayout;
    CGRect _lastLayoutArea;

    CADisplayLink *_displayLink;
    NSTimeInterval _lastTickTime;

    uint32_t _touchMask;
    uint32_t _keyboardMask;
    uint32_t _controllerMask;
    NSMutableIndexSet *_pressedKeys;

    BOOL _foreground;
    BOOL _userPaused;
    BOOL _menuOpen;
    BOOL _audioRunning;
    BOOL _audioInterrupted;
#if GP32EMU_IOS_DEVELOPER_JIT
    BOOL _jitPending, _jitTransitioning;
    NSUInteger _jitRequest;
    NSTimer *_jitWaitTimer;
    CFTimeInterval _jitDeadline;
    UIBackgroundTaskIdentifier _jitBackgroundTask;
#endif
    BOOL _controllerMenuPressed;
    BOOL _importing;
    BOOL _restoredSelection;

    NSString *_gamePath;
    NSString *_biosPath;
    gp32_ios_boot_mode_t _bootMode;
    GP32ImportKind _pendingImport;

    BOOL _hasKeyboardObserver;
    NSArray<id> *_controllerObservers;
}

#pragma mark - Storage

+ (NSString *)documentsDirectory {
    return [NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES) firstObject];
}

+ (BOOL)ensureDirectory:(NSString *)path {
    NSError *error = nil;
    if ([[NSFileManager defaultManager] createDirectoryAtPath:path
                                  withIntermediateDirectories:YES
                                           attributes:nil
                                                error:&error]) {
        return YES;
    }
    NSLog(@"gp32emu ios: cannot create %@: %@", path, error.localizedDescription);
    return NO;
}

/* Imported content lives in Documents/GP32emu/<kind> so the user can manage it
 * in Files; card progress lives in Library/Application Support/GP32emu/Cards
 * because it is app-owned state, not an import. Nothing is copied into the
 * bundle: no BIOS image and no game image ships with this app. */
+ (NSString *)gamesDirectory {
    return [[self documentsDirectory] stringByAppendingPathComponent:@"GP32emu/Games"];
}

+ (NSString *)biosDirectory {
    return [[self documentsDirectory] stringByAppendingPathComponent:@"GP32emu/BIOS"];
}

+ (NSString *)statesDirectory {
    return [[self documentsDirectory] stringByAppendingPathComponent:@"GP32emu/States"];
}

+ (NSString *)cardsDirectory {
    return [[[NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES) firstObject]
             stringByAppendingPathComponent:@"GP32emu"] stringByAppendingPathComponent:@"Cards"];
}

- (gp32_ios_boot_mode_t)storedBootMode {
    NSInteger stored = [[NSUserDefaults standardUserDefaults] integerForKey:GP32DefaultBootModeKey];
    if (stored < (NSInteger)GP32_IOS_BOOT_AUTO || stored > (NSInteger)GP32_IOS_BOOT_DIRECT) {
        return GP32_IOS_BOOT_AUTO;
    }
    return (gp32_ios_boot_mode_t)stored;
}

#pragma mark - Lifecycle

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = [UIColor blackColor];
    _foreground = YES;
    _bootMode = [self storedBootMode];
    _pressedKeys = [[NSMutableIndexSet alloc] init];
    _padButtons = [[NSMutableArray alloc] init];

    [self buildInterface];

    for (NSString *directory in @[[GP32EmulatorViewController gamesDirectory],
                                  [GP32EmulatorViewController biosDirectory],
                                  [GP32EmulatorViewController statesDirectory],
                                  [GP32EmulatorViewController cardsDirectory]]) {
        [GP32EmulatorViewController ensureDirectory:directory];
    }

    _audio = [[GP32Audio alloc] init];
#if GP32EMU_IOS_DEVELOPER_JIT
    _jitBackgroundTask = UIBackgroundTaskInvalid;
#endif
    /* The emulator retains the audio object: queued owner-queue blocks may
     * still write the sink after this controller is gone. */
    _emulator = [[GP32Emulator alloc] initWithAudio:_audio];

    __weak typeof(self) weakSelf = self;
    _emulator.frameHandler = ^(NSData *pixels, uint64_t frameCounter) {
        (void)frameCounter;
        [weakSelf presentPixels:pixels];
    };
    _emulator.statusHandler = ^(NSString *message, BOOL temporary) {
        typeof(self) self = weakSelf;
        if (self) {
            BOOL running = [message hasPrefix:@"Running "];
            [self showStatus:running ? nil : message temporary:temporary];
            if (running) [self revealMenuButton];
        }
    };

    [self installKeyboardInput];
    [self installControllerInput];
    [self installDisplayLink];

    NSNotificationCenter *center = [NSNotificationCenter defaultCenter];
    [center addObserver:self selector:@selector(applicationDidBecomeActive:)
                   name:UIApplicationDidBecomeActiveNotification object:nil];
    [center addObserver:self selector:@selector(applicationWillResignActive:)
                   name:UIApplicationWillResignActiveNotification object:nil];
    [center addObserver:self selector:@selector(applicationDidEnterBackground:)
                   name:UIApplicationDidEnterBackgroundNotification object:nil];
    /* Keep observing while a full-screen picker covers the controller too. */
    for (NSNotificationName name in @[AVAudioSessionInterruptionNotification,
                                     AVAudioEngineConfigurationChangeNotification,
                                     AVAudioSessionRouteChangeNotification,
                                     AVAudioSessionMediaServicesWereResetNotification]) {
        [center addObserver:self selector:@selector(handleAudioSystemNotification:)
                       name:name object:nil];
    }
}

- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    self.view.window.backgroundColor = [UIColor blackColor];

    if (!_restoredSelection) {
        _restoredSelection = YES;
        [self restoreSelectionIfPossible];
    }
}

- (void)dealloc {
#if GP32EMU_IOS_DEVELOPER_JIT
    [_jitWaitTimer invalidate];
    if (_jitPending && _jitBackgroundTask != UIBackgroundTaskInvalid)
        [UIApplication.sharedApplication endBackgroundTask:_jitBackgroundTask];
#endif
    [_statusHideTimer invalidate];
    [_menuHideTimer invalidate];
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [_displayLink invalidate];
    _displayLink = nil;
    [_emulator pause];
    [_emulator releaseInput];
    [_audio stop];
    [self stopControllerInput];
    [self stopKeyboardInput];
}

#pragma mark - Interface

- (void)showStatus:(NSString *)message temporary:(BOOL)temporary {
    [_statusHideTimer invalidate];
    _statusHideTimer = nil;
    _statusLabel.text = message;
    if (!temporary || !message.length) return;
    __weak typeof(self) weakSelf = self;
    _statusHideTimer = [NSTimer timerWithTimeInterval:2.0 repeats:NO block:^(NSTimer *timer) {
        (void)timer;
        typeof(self) self = weakSelf;
        if (!self) return;
        self->_statusHideTimer = nil;
        self->_statusLabel.text = nil;
    }];
    [[NSRunLoop mainRunLoop] addTimer:_statusHideTimer forMode:NSRunLoopCommonModes];
}

- (UILabel *)makeCaption:(NSString *)text size:(CGFloat)size bold:(BOOL)bold {
    UILabel *label = [[UILabel alloc] initWithFrame:CGRectZero];
    label.text = text;
    label.textColor = [UIColor colorWithWhite:0.9 alpha:1.0];
    label.font = bold ? [UIFont systemFontOfSize:size weight:UIFontWeightSemibold]
                      : [UIFont systemFontOfSize:size];
    label.textAlignment = NSTextAlignmentCenter;
    label.translatesAutoresizingMaskIntoConstraints = NO;
    return label;
}

/* A separate control, not a game button: its only gesture is a completed tap
 * on itself, so no other active touch can open the menu. */
- (UIButton *)makeMenuButton {
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    button.accessibilityLabel = @"Menu";
    button.accessibilityHint = @"Pause the game and open options.";
    /* Fade the artwork, not the button: alpha-zero UIViews stop hit testing.
     * The clear target must still open the menu after the icon hides. */
    _menuGlyph = [[UIView alloc] initWithFrame:CGRectMake(6, 6, 32, 32)];
    _menuGlyph.userInteractionEnabled = NO;
    _menuGlyph.backgroundColor = [UIColor colorWithWhite:0.15 alpha:0.8];
    _menuGlyph.layer.cornerRadius = 16;
    UIImageSymbolConfiguration *symbol = [UIImageSymbolConfiguration
        configurationWithPointSize:20 weight:UIImageSymbolWeightSemibold];
    UIImageView *icon = [[UIImageView alloc] initWithImage:
        [UIImage systemImageNamed:@"ellipsis" withConfiguration:symbol]];
    icon.tintColor = UIColor.whiteColor;
    icon.contentMode = UIViewContentModeScaleAspectFit;
    icon.frame = CGRectMake(6, 6, 20, 20);
    [_menuGlyph addSubview:icon];
    [button addSubview:_menuGlyph];
    button.translatesAutoresizingMaskIntoConstraints = NO;
    [button addTarget:self action:@selector(showMenu:) forControlEvents:UIControlEventTouchUpInside];
    return button;
}

- (void)revealMenuButton {
    [_menuHideTimer invalidate];
    _menuHideTimer = nil;
    [_menuGlyph.layer removeAllAnimations];
    _menuGlyph.alpha = 1.0;
    if (!_landscapeLayout || !_emulator.hasGame || _menuOpen) return;
    __weak typeof(self) weakSelf = self;
    _menuHideTimer = [NSTimer timerWithTimeInterval:3.0 repeats:NO block:^(NSTimer *timer) {
        (void)timer;
        typeof(self) self = weakSelf;
        if (!self) return;
        self->_menuHideTimer = nil;
        [UIView animateWithDuration:0.25 animations:^{ self->_menuGlyph.alpha = 0.0; }];
    }];
    [[NSRunLoop mainRunLoop] addTimer:_menuHideTimer forMode:NSRunLoopCommonModes];
}

- (GP32PadButton *)addButtonBit:(uint32_t)bit caption:(NSString *)caption to:(UIView *)parent {
    GP32PadButton *button = [[GP32PadButton alloc] initWithBit:bit caption:caption];
    button.translatesAutoresizingMaskIntoConstraints = NO;
    __weak typeof(self) weakSelf = self;
    button.pressHandler = ^{
        typeof(self) self = weakSelf;
        if (self) [self setTouchMask:self->_touchMask | bit];
    };
    button.releaseHandler = ^{
        typeof(self) self = weakSelf;
        if (self) [self setTouchMask:self->_touchMask & ~bit];
    };
    [parent addSubview:button];
    [_padButtons addObject:button];
    return button;
}

- (void)buildInterface {
    _pictureView = [[UIView alloc] initWithFrame:CGRectZero];
    _pictureView.backgroundColor = UIColor.blackColor;
    _pictureView.layer.contentsGravity = kCAGravityResize;
    _pictureView.layer.magnificationFilter = kCAFilterNearest;
    _pictureView.accessibilityLabel = @"GP32 screen";
    _pictureView.isAccessibilityElement = YES;
    [self.view addSubview:_pictureView];

    _statusLabel = [self makeCaption:@"Import a game image from the menu to begin." size:14.0 bold:NO];
    _statusLabel.translatesAutoresizingMaskIntoConstraints = YES;
    _statusLabel.numberOfLines = 0;
    _statusLabel.userInteractionEnabled = NO;
    _statusLabel.shadowColor = UIColor.blackColor;
    _statusLabel.shadowOffset = CGSizeMake(0, 1);
    [self.view addSubview:_statusLabel];

    _leftShoulder = [self addButtonBit:GP32_BUTTON_L caption:@"L" to:self.view];
    _rightShoulder = [self addButtonBit:GP32_BUTTON_R caption:@"R" to:self.view];
    _selectButton = [self addButtonBit:GP32_BUTTON_SELECT caption:@"Select" to:self.view];
    _startButton = [self addButtonBit:GP32_BUTTON_START caption:@"Start" to:self.view];
    _buttonA = [self addButtonBit:GP32_BUTTON_A caption:@"A" to:self.view];
    _buttonB = [self addButtonBit:GP32_BUTTON_B caption:@"B" to:self.view];
    for (GP32PadButton *button in _padButtons) {
        button.translatesAutoresizingMaskIntoConstraints = YES;
        button.captionLabel.adjustsFontSizeToFitWidth = YES;
        button.captionLabel.minimumScaleFactor = 0.8;
    }
    _buttonA.captionLabel.font = [UIFont systemFontOfSize:22 weight:UIFontWeightBold];
    _buttonB.captionLabel.font = _buttonA.captionLabel.font;

    _pad = [[GP32PadView alloc] initWithFrame:CGRectZero];
    __weak typeof(self) weakSelf = self;
    _pad.maskHandler = ^(uint32_t mask) {
        typeof(self) self = weakSelf;
        if (self) [self setTouchMask:(self->_touchMask & ~0x3C0u) | mask];
    };
    [self.view addSubview:_pad];
    _menuButton = [self makeMenuButton];
    _menuButton.translatesAutoresizingMaskIntoConstraints = YES;
    [self.view addSubview:_menuButton];
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    CGRect safe = UIEdgeInsetsInsetRect(self.view.bounds, self.view.safeAreaInsets);
    if (CGRectIsEmpty(safe)) return;
    BOOL layoutChanged = !CGRectEqualToRect(safe, _lastLayoutArea);
    if (layoutChanged) {
        /* A rotation must not move a held target underneath the same finger.
         * Release touch input only; hardware inputs keep their own state. */
        for (GP32PadButton *button in _padButtons) {
            [button cancelTrackingWithEvent:nil];
            [button cancelPress];
        }
        [_pad cancelTrackingWithEvent:nil];
        [_pad resetMask];
        [_menuButton cancelTrackingWithEvent:nil];
        [self setTouchMask:0];
        _lastLayoutArea = safe;
    }
    CGFloat margin = 12, gap = 12;
    CGFloat left = CGRectGetMinX(safe) + margin;
    CGFloat right = CGRectGetMaxX(safe) - margin;
    CGFloat top = CGRectGetMinY(safe) + 8;
    CGFloat bottom = CGRectGetMaxY(safe) - 8;
    CGFloat width = right - left;
    BOOL landscape = safe.size.width > safe.size.height;
    _landscapeLayout = landscape;
    CGFloat padSize;
    CGRect pictureArea;
    if (landscape) {
        /* Keep Start/Select away from the shoulder buttons: they occupy the
         * lower center, while the d-pad and A/B stay in the side columns. */
        padSize = MIN(144, MIN((width - 2 * gap) / 4, (bottom - top) - 164));
        padSize = MAX(72, padSize);
        CGFloat column = MAX(96, padSize);
        _pad.frame = CGRectMake(left, bottom - padSize, padSize, padSize);
        CGFloat small = MIN(88, column);
        CGFloat leftButton = left + (column - small) / 2;
        CGFloat rightButton = right - (column + small) / 2;
        _leftShoulder.frame = CGRectMake(leftButton, top, small, 44);
        _rightShoulder.frame = CGRectMake(rightButton, top, small, 44);
        _selectButton.frame = CGRectMake(CGRectGetMidX(safe) - gap - 72, bottom - 44, 72, 44);
        _startButton.frame = CGRectMake(CGRectGetMidX(safe) + gap, bottom - 44, 72, 44);
        pictureArea = CGRectMake(left + column + gap, top,
                                 MAX(1, width - 2 * (column + gap)), MAX(1, bottom - top - 56));
    } else {
        padSize = MIN(156, (width - gap) / 2);
        CGFloat controlsTop = bottom - padSize - 56;
        _pad.frame = CGRectMake(left, bottom - padSize, padSize, padSize);
        CGFloat small = MIN(72, (width - 3 * gap) / 4);
        _leftShoulder.frame = CGRectMake(left, controlsTop, small, 44);
        _rightShoulder.frame = CGRectMake(right - small, controlsTop, small, 44);
        _selectButton.frame = CGRectMake(CGRectGetMidX(safe) - gap / 2 - small, controlsTop, small, 44);
        _startButton.frame = CGRectMake(CGRectGetMidX(safe) + gap / 2, controlsTop, small, 44);
        _menuButton.frame = CGRectMake(right - 44, top, 44, 44);
        pictureArea = CGRectMake(left, top + 56, width, MAX(1, controlsTop - top - 68));
    }
    /* A upper-right, B lower-left, as on Android. Both have independent
     * touch tracking, so direction + A/B and simultaneous buttons work. */
    CGFloat actionSize = padSize * 0.45;
    _buttonA.frame = CGRectMake(right - actionSize, bottom - padSize, actionSize, actionSize);
    _buttonB.frame = CGRectMake(right - padSize, bottom - actionSize, actionSize, actionSize);
    _buttonA.layer.cornerRadius = actionSize / 2;
    _buttonB.layer.cornerRadius = actionSize / 2;
    CGFloat pictureWidth = MIN(pictureArea.size.width, pictureArea.size.height * 4.0 / 3.0);
    CGFloat pictureHeight = pictureWidth * 3.0 / 4.0;
    _pictureView.frame = CGRectIntegral(CGRectMake(CGRectGetMidX(pictureArea) - pictureWidth / 2,
                                                  landscape ? CGRectGetMinY(pictureArea) :
                                                      CGRectGetMidY(pictureArea) - pictureHeight / 2,
                                                  pictureWidth, pictureHeight));
    _statusLabel.frame = CGRectInset(_pictureView.frame, 16, 16);
    if (landscape) {
        _menuButton.frame = CGRectMake(CGRectGetMidX(_pictureView.frame) - 36,
                                      CGRectGetMinY(_pictureView.frame), 72, 56);
    }
    /* Keep the 32-point artwork in place while widening the clear touch target. */
    _menuGlyph.frame = CGRectMake((CGRectGetWidth(_menuButton.bounds) - 32) / 2, 6, 32, 32);
    if (layoutChanged) [self revealMenuButton];
}

#pragma mark - Frame presentation

/* Builds one 320x240 CGImage from the snapshot the emulator copied on its
 * owner queue. The image retains that data, so nothing borrowed from the core
 * is read here. */
- (void)presentPixels:(NSData *)pixels {
    if (pixels.length != GP32_IOS_FRAME_PIXELS * sizeof(uint32_t)) return;
    CGDataProviderRef provider = CGDataProviderCreateWithCFData((__bridge CFDataRef)pixels);
    if (!provider) return;
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    /* The bridge words are 0xAARRGGBB, so the bytes are B,G,R,A on a
     * little-endian device: 32-bit little-endian BGRA with no alpha use. */
    CGImageRef image = CGImageCreate((size_t)GP32_IOS_FRAME_WIDTH,
                                     (size_t)GP32_IOS_FRAME_HEIGHT,
                                     8, 32, (size_t)GP32_IOS_FRAME_WIDTH * 4u,
                                     space,
                                     (CGBitmapInfo)(kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst),
                                     provider, NULL, false, kCGRenderingIntentDefault);
    if (image) _pictureView.layer.contents = (__bridge_transfer id)image;
    CGColorSpaceRelease(space);
    CGDataProviderRelease(provider);
}

#pragma mark - Input combination

- (void)setTouchMask:(uint32_t)touchMask {
    if (_touchMask == touchMask) return;
    _touchMask = touchMask;
    [self pushButtonMask];
}

- (void)setKeyboardMask:(uint32_t)keyboardMask {
    if (_keyboardMask == keyboardMask) return;
    _keyboardMask = keyboardMask;
    [self pushButtonMask];
}

- (void)setControllerMask:(uint32_t)controllerMask {
    if (_controllerMask == controllerMask) return;
    _controllerMask = controllerMask;
    [self pushButtonMask];
}

/* Touch, keyboard and controller states are independent; the guest sees their
 * union, so releasing one source never drops a button another still holds. */
- (void)pushButtonMask {
    [_emulator setButtonMask:(_touchMask | _keyboardMask | _controllerMask)];
}

- (void)clearInputState {
    _touchMask = 0u;
    _keyboardMask = 0u;
    _controllerMask = 0u;
    for (GP32PadButton *button in _padButtons) [button cancelPress];
    [_pad resetMask];
    [_pressedKeys removeAllIndexes];
}

#pragma mark - Keyboard and controller

- (void)installKeyboardInput {
    if (@available(iOS 14.0, *)) {
        GCKeyboard *keyboard = GCKeyboard.coalescedKeyboard;
        if (!keyboard) return;
        __weak typeof(self) weakSelf = self;
        keyboard.keyboardInput.keyChangedHandler =
            ^(GCKeyboardInput *input, GCDeviceButtonInput *key, GCKeyCode code, BOOL pressed) {
                (void)input;
                (void)key;
                [weakSelf handleKeyCode:code pressed:pressed];
            };
        _hasKeyboardObserver = YES;
    }
}

- (void)stopKeyboardInput {
    if (!_hasKeyboardObserver) return;
    if (@available(iOS 14.0, *)) {
        GCKeyboard.coalescedKeyboard.keyboardInput.keyChangedHandler = nil;
        _hasKeyboardObserver = NO;
    }
}

- (void)handleKeyCode:(GCKeyCode)code pressed:(BOOL)pressed {
    if (pressed) [_pressedKeys addIndex:(NSUInteger)code];
    else [_pressedKeys removeIndex:(NSUInteger)code];

    uint32_t mask = 0;
    if ([_pressedKeys containsIndex:(NSUInteger)GCKeyCodeUpArrow]) mask |= GP32_BUTTON_UP;
    if ([_pressedKeys containsIndex:(NSUInteger)GCKeyCodeDownArrow]) mask |= GP32_BUTTON_DOWN;
    if ([_pressedKeys containsIndex:(NSUInteger)GCKeyCodeLeftArrow]) mask |= GP32_BUTTON_LEFT;
    if ([_pressedKeys containsIndex:(NSUInteger)GCKeyCodeRightArrow]) mask |= GP32_BUTTON_RIGHT;
    if ([_pressedKeys containsIndex:(NSUInteger)GCKeyCodeKeyZ]) mask |= GP32_BUTTON_A;
    if ([_pressedKeys containsIndex:(NSUInteger)GCKeyCodeKeyX]) mask |= GP32_BUTTON_B;
    if ([_pressedKeys containsIndex:(NSUInteger)GCKeyCodeKeyA]) mask |= GP32_BUTTON_L;
    if ([_pressedKeys containsIndex:(NSUInteger)GCKeyCodeKeyS]) mask |= GP32_BUTTON_R;
    if ([_pressedKeys containsIndex:(NSUInteger)GCKeyCodeReturnOrEnter]) mask |= GP32_BUTTON_START;
    if ([_pressedKeys containsIndex:(NSUInteger)GCKeyCodeLeftShift] ||
        [_pressedKeys containsIndex:(NSUInteger)GCKeyCodeRightShift]) mask |= GP32_BUTTON_SELECT;
    [self setKeyboardMask:mask];

    if (pressed && code == GCKeyCodeEscape) [self showMenu:nil];
}

- (void)installControllerInput {
    NSNotificationCenter *center = [NSNotificationCenter defaultCenter];
    __weak typeof(self) weakSelf = self;
    _controllerObservers = @[
        [center addObserverForName:GCControllerDidConnectNotification object:nil
                             queue:[NSOperationQueue mainQueue]
                        usingBlock:^(NSNotification *note) { [weakSelf refreshControllers]; }],
        [center addObserverForName:GCControllerDidDisconnectNotification object:nil
                             queue:[NSOperationQueue mainQueue]
                        usingBlock:^(NSNotification *note) { [weakSelf refreshControllers]; }],
    ];
    [self refreshControllers];
}

- (void)stopControllerInput {
    for (GCController *controller in GCController.controllers) {
        controller.extendedGamepad.valueChangedHandler = nil;
    }
    for (id observer in _controllerObservers) {
        [[NSNotificationCenter defaultCenter] removeObserver:observer];
    }
    _controllerObservers = nil;
}

- (void)refreshControllers {
    __weak typeof(self) weakSelf = self;
    uint32_t mask = 0;
    BOOL menuPressed = NO;
    for (GCController *controller in GCController.controllers) {
        GCExtendedGamepad *pad = controller.extendedGamepad;
        if (!pad) continue;
        controller.handlerQueue = dispatch_get_main_queue();
        menuPressed |= pad.buttonMenu.isPressed;
        pad.valueChangedHandler = ^(GCExtendedGamepad *gamepad, GCControllerElement *element) {
            (void)element;
            [weakSelf handleController:gamepad];
        };
        mask = [self controllerBitsForPad:pad intoMask:mask];
    }
    [self setControllerMask:mask];
    _controllerMenuPressed = menuPressed;
}

- (uint32_t)controllerBitsForPad:(GCExtendedGamepad *)pad intoMask:(uint32_t)mask {
    if (pad.buttonA.isPressed) mask |= GP32_BUTTON_A;
    if (pad.buttonB.isPressed) mask |= GP32_BUTTON_B;
    if (pad.leftShoulder.isPressed) mask |= GP32_BUTTON_L;
    if (pad.rightShoulder.isPressed) mask |= GP32_BUTTON_R;
    if (pad.dpad.up.isPressed) mask |= GP32_BUTTON_UP;
    if (pad.dpad.down.isPressed) mask |= GP32_BUTTON_DOWN;
    if (pad.dpad.left.isPressed) mask |= GP32_BUTTON_LEFT;
    if (pad.dpad.right.isPressed) mask |= GP32_BUTTON_RIGHT;
    if (pad.buttonX.isPressed) mask |= GP32_BUTTON_START;
    if (pad.buttonY.isPressed) mask |= GP32_BUTTON_SELECT;
    if (@available(iOS 13.0, *)) {
        if (pad.buttonOptions.isPressed) mask |= GP32_BUTTON_SELECT;
    }
    return mask;
}

- (void)handleController:(GCExtendedGamepad *)pad {
    (void)pad;
    uint32_t mask = 0;
    BOOL menuPressed = NO;
    for (GCController *controller in GCController.controllers) {
        GCExtendedGamepad *current = controller.extendedGamepad;
        if (!current) continue;
        mask = [self controllerBitsForPad:current intoMask:mask];
        menuPressed |= current.buttonMenu.isPressed;
    }
    [self setControllerMask:mask];

    /* Only a fresh press of the controller's own menu button opens this app's
     * menu; a held button never repeats it. */
    if (menuPressed && !_controllerMenuPressed) [self showMenu:nil];
    _controllerMenuPressed = menuPressed;
}

#pragma mark - Frame pacing

- (void)installDisplayLink {
    if (_displayLink) return;
    _displayLink = [CADisplayLink displayLinkWithTarget:self selector:@selector(displayTick:)];
    /* Ask for exactly one callback per emulated interval on every panel. */
    _displayLink.preferredFrameRateRange = CAFrameRateRangeMake(60.0, 60.0, 60.0);
    _lastTickTime = 0.0;
    [_displayLink addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
}

/* Preserve the 60 Hz guest clock across short display delays. Lifecycle and
 * menu transitions reset the clock; ordinary jitter can owe one extra frame. */
- (void)displayTick:(CADisplayLink *)link {
    if (![self shouldRun]) return;
    if (!_emulator.hasGame) {
        _lastTickTime = 0.0;
        return;
    }
    NSTimeInterval now = link.timestamp;
    if (_lastTickTime == 0.0) {
        _lastTickTime = now;
        [_emulator tick];
        return;
    }
    if (now + GP32DisplayIntervalSlack < _lastTickTime + GP32DisplayInterval) return;
    NSUInteger due = (NSUInteger)((now - _lastTickTime + GP32DisplayIntervalSlack) /
                                  GP32DisplayInterval);
    if (due > 2) {
        /* A long suspension must not cause an unbounded fast-forward burst. */
        due = 2;
        _lastTickTime = now;
    } else {
        /* Retain fractional time: rounding the deadline up to each callback
         * permanently loses guest time and PCM when the display runs late. */
        _lastTickTime += due * GP32DisplayInterval;
    }
    for (NSUInteger index = 0; index < due; ++index) [_emulator tick];
}

#pragma mark - Running state

- (BOOL)shouldRun {
#if GP32EMU_IOS_DEVELOPER_JIT
    if (_jitPending) return NO;
#endif
    return _foreground && !_userPaused && !_menuOpen && !_importing &&
           !_audioInterrupted && _emulator.hasGame;
}

/* The one place the app starts or stops the guest and the audio graph, so a
 * load, a menu, a background transition and an interruption cannot leave the
 * two disagreeing. */
- (void)applyRunningState {
    if ([self shouldRun]) {
        if (!_audioRunning || !_audio.isRunning) {
            NSError *error = nil;
            _audioRunning = [_audio startWithError:&error];
            if (!_audioRunning) {
                [self showStatus:error.localizedDescription ?: @"Audio is unavailable; the game keeps running." temporary:NO];
            }
        }
        [_emulator start];
        [self pushButtonMask];
        return;
    }

    [_emulator pause];
    [_emulator releaseInput];
    [self clearInputState];
    if (_audioRunning) {
        [_audio stop];
        _audioRunning = NO;
    }
}

#pragma mark - Content

/* iOS can move the data container when updating an app. Persist app-relative
 * paths and also recover selections saved by older builds with the old UUID. */
+ (NSString *)storedContentPath:(NSString *)path {
    NSString *prefix = [NSHomeDirectory() stringByAppendingString:@"/"];
    return [path hasPrefix:prefix] ? [path substringFromIndex:prefix.length] : path;
}

+ (NSString *)resolveContentPath:(NSString *)stored {
    if (!stored.length) return nil;
    NSFileManager *files = NSFileManager.defaultManager;
    if (stored.isAbsolutePath && [files fileExistsAtPath:stored]) return stored;
    NSString *relative = stored;
    if (stored.isAbsolutePath) {
        NSRange marker = [stored rangeOfString:@"/Documents/GP32emu/"];
        if (marker.location == NSNotFound) return nil;
        relative = [stored substringFromIndex:marker.location + 1];
    }
    if (![relative hasPrefix:@"Documents/GP32emu/"]) return nil;
    NSString *path = [[NSHomeDirectory() stringByAppendingPathComponent:relative] stringByStandardizingPath];
    NSString *prefix = [[self documentsDirectory] stringByAppendingString:@"/GP32emu/"];
    return [path hasPrefix:prefix] && [files fileExistsAtPath:path] ? path : nil;
}

- (void)restoreSelectionIfPossible {
    NSUserDefaults *defaults = [NSUserDefaults standardUserDefaults];
    _biosPath = [GP32EmulatorViewController resolveContentPath:[defaults stringForKey:GP32DefaultBIOSKey]];
    NSString *game = [GP32EmulatorViewController resolveContentPath:[defaults stringForKey:GP32DefaultGameKey]];
    if (game) [self loadContentAtPath:game];
}

- (void)loadContentAtPath:(NSString *)gamePath {
    NSFileManager *files = [NSFileManager defaultManager];
    if (![files fileExistsAtPath:gamePath]) {
        [self showStatus:[NSString stringWithFormat:@"%@ is no longer available.",
                         gamePath.lastPathComponent] temporary:NO];
        return;
    }

#if GP32EMU_IOS_DEVELOPER_JIT
    if (_jitPending) [self finishExternalJIT:@"JIT request cancelled because the game changed."];
#endif
    _userPaused = NO;

    NSString *name = gamePath.lastPathComponent;
    NSString *identity = gamePath.stringByDeletingLastPathComponent.lastPathComponent;
    NSString *cardPath = [[GP32EmulatorViewController cardsDirectory]
                 stringByAppendingPathComponent:[identity stringByAppendingPathExtension:@"sav"]];
    NSString *statePath = [[GP32EmulatorViewController statesDirectory]
                  stringByAppendingPathComponent:[identity stringByAppendingPathExtension:@"gp32st"]];

    NSUserDefaults *defaults = [NSUserDefaults standardUserDefaults];
    if (_biosPath) [defaults setObject:[GP32EmulatorViewController storedContentPath:_biosPath] forKey:GP32DefaultBIOSKey];

    /* A load restarts the guest: stop playback and discard queued PCM through
     * the sink's own flush protocol before the core is swapped. */
    [_audio stop];
    _audioRunning = NO;

    __weak typeof(self) weakSelf = self;
    [_emulator loadContentAtPath:gamePath
                        biosPath:_biosPath
                           label:name
                    cardSavePath:cardPath
                       statePath:statePath
                        bootMode:_bootMode
                      completion:^(BOOL success, NSString *message) {
                          (void)message;
                          typeof(self) self = weakSelf;
                          if (!self) return;
                          self->_gamePath = [self->_emulator.contentPath copy];
                          if (self->_gamePath) {
                              [[NSUserDefaults standardUserDefaults]
                                  setObject:[GP32EmulatorViewController storedContentPath:self->_gamePath]
                                     forKey:GP32DefaultGameKey];
                          }
                          self->_userPaused = !success;
                          self->_lastTickTime = 0.0;
                          [self applyRunningState];
                      }];
}

- (void)chooseBootMode:(gp32_ios_boot_mode_t)mode {
    _bootMode = mode;
    [[NSUserDefaults standardUserDefaults] setInteger:(NSInteger)_bootMode
                                                forKey:GP32DefaultBootModeKey];
    [self showStatus:@"Boot mode set. Reload the game to apply it." temporary:NO];
    [self applyRunningState];
}

#pragma mark - Document import

- (void)presentPickerForImport:(GP32ImportKind)kind {
    _pendingImport = kind;
    UIDocumentPickerViewController *picker =
        [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[UTTypeData] asCopy:YES];
    picker.delegate = self;
    picker.allowsMultipleSelection = NO;
    [self presentSoon:picker];
}

/* An alert action handler runs while its own sheet is still being dismissed, so
 * the follow-up controller goes out on the next main-queue turn instead of
 * racing that dismissal. */
- (void)presentSoon:(UIViewController *)controller {
    __weak typeof(self) weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        typeof(self) self = weakSelf;
        if (self) [self presentViewController:controller animated:YES completion:nil];
    });
}

/* The picker hands out a security-scoped URL; the copy into private app
 * storage happens between the access pair and the scope is always released. */
- (void)documentPicker:(UIDocumentPickerViewController *)controller
        didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    (void)controller;
    NSURL *source = urls.firstObject;
    if (!source) {
        [self menuDidClose];
        return;
    }

    GP32ImportKind kind = _pendingImport;
    NSString *directory = (kind == GP32ImportBIOS)
        ? [GP32EmulatorViewController biosDirectory]
        : [GP32EmulatorViewController gamesDirectory];
    _importing = YES;
    [self showStatus:@"Importing…" temporary:NO];
    __weak typeof(self) weakSelf = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        BOOL scoped = [source startAccessingSecurityScopedResource];
        NSError *error = nil;
        NSString *destination = [GP32EmulatorViewController importURL:source into:directory error:&error];
        if (scoped) [source stopAccessingSecurityScopedResource];
        dispatch_async(dispatch_get_main_queue(), ^{
            typeof(self) self = weakSelf;
            if (!self) return;
            self->_importing = NO;
            if (!destination) {
                [self showStatus:[NSString stringWithFormat:@"Import failed: %@", error.localizedDescription ?: @"Cannot store the file."] temporary:NO];
            } else if (kind == GP32ImportBIOS) {
                self->_biosPath = destination;
                [[NSUserDefaults standardUserDefaults] setObject:[GP32EmulatorViewController storedContentPath:destination] forKey:GP32DefaultBIOSKey];
                [self showStatus:@"BIOS imported. Reload the game to boot with it." temporary:NO];
            } else {
                [self loadContentAtPath:destination];
            }
            [self menuDidClose];
        });
    });
}

+ (NSString *)importURL:(NSURL *)source into:(NSString *)directory error:(NSError **)error {
    NSInputStream *stream = [NSInputStream inputStreamWithURL:source];
    [stream open];
    CC_SHA256_CTX hash;
    CC_SHA256_Init(&hash);
    uint8_t buffer[65536], digest[CC_SHA256_DIGEST_LENGTH];
    NSInteger read;
    NSUInteger total = 0;
    while ((read = [stream read:buffer maxLength:sizeof(buffer)]) > 0) {
        CC_SHA256_Update(&hash, buffer, (CC_LONG)read);
        total += (NSUInteger)read;
    }
    NSError *readError = stream.streamError;
    [stream close];
    if (read < 0 || readError || total == 0) {
        if (error) *error = readError ?: [NSError errorWithDomain:NSCocoaErrorDomain code:NSFileReadCorruptFileError userInfo:nil];
        return nil;
    }
    CC_SHA256_Final(digest, &hash);
    NSMutableString *identity = [NSMutableString stringWithCapacity:64];
    for (NSUInteger i = 0; i < sizeof(digest); ++i) [identity appendFormat:@"%02x", digest[i]];
    NSString *folder = [directory stringByAppendingPathComponent:identity];
    NSFileManager *files = NSFileManager.defaultManager;
    if (![files createDirectoryAtPath:folder withIntermediateDirectories:YES attributes:nil error:error]) return nil;
    NSString *destination = [folder stringByAppendingPathComponent:source.lastPathComponent];
    if ([files fileExistsAtPath:destination]) return destination;
    // Copy to a fresh temporary file; failure never replaces a prior import.
    NSString *temporary = [folder stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
    if (![files copyItemAtPath:source.path toPath:temporary error:error]) {
        [files removeItemAtPath:temporary error:NULL];
        return nil;
    }
    if (![files moveItemAtPath:temporary toPath:destination error:error]) {
        [files removeItemAtPath:temporary error:NULL];
        return nil;
    }
    return destination;
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)controller {
    (void)controller;
    [self menuDidClose];
}

#pragma mark - Menu

- (void)showMenu:(id)sender {
    if ([sender isKindOfClass:UIButton.class]) {
        if (_pad.isTracking) return;
        for (GP32PadButton *button in _padButtons) if (button.isTracking) return;
    }
    /* One menu at a time; a second request while a sheet is open is ignored. */
    if (_menuOpen || _importing || self.presentedViewController) return;
    _menuOpen = YES;
    [self revealMenuButton];
    /* Pausing stops the guest, releases all input and stops the audio graph;
     * its ring discards queued PCM through the consumer-owned flush. */
    [self applyRunningState];
    _displayLink.paused = YES;

    NSString *menuMessage = _emulator.contentLabel;
#if GP32EMU_IOS_DEVELOPER_JIT
    if (_emulator.hasGame) {
        menuMessage = [NSString stringWithFormat:@"%@\n%@", menuMessage ?: @"",
                       _emulator.jitActive ? @"JIT enabled" : @"Interpreter"];
    }
#endif
    UIAlertController *menu = [UIAlertController alertControllerWithTitle:@"GP32emu"
                                                                  message:menuMessage
                                                           preferredStyle:UIAlertControllerStyleActionSheet];
    __weak typeof(self) weakSelf = self;

#if GP32EMU_IOS_DEVELOPER_JIT
    UIAlertAction *jitAction = [UIAlertAction actionWithTitle:(_jitPending ? @"Cancel JIT request" : @"Enable JIT with StikDebug")
        style:UIAlertActionStyleDefault handler:^(UIAlertAction *action) {
            (void)action;
            typeof(self) self = weakSelf;
            if (!self) return;
            [self menuDidClose];
            if (self->_jitPending) [self finishExternalJIT:@"JIT request cancelled. Interpreter retained."];
            else [self requestExternalJIT];
        }];
    jitAction.enabled = _emulator.hasGame && !_emulator.jitActive && !_jitTransitioning;
    [menu addAction:jitAction];
#endif

    [menu addAction:[UIAlertAction actionWithTitle:@"Import game image"
                                            style:UIAlertActionStyleDefault
                                          handler:^(UIAlertAction *action) {
        (void)action;
        /* The picker keeps the menu session open (and the guest paused) until
         * it finishes; its delegate calls menuDidClose. */
        [weakSelf presentPickerForImport:GP32ImportGame];
    }]];
    [menu addAction:[UIAlertAction actionWithTitle:@"Import BIOS image"
                                            style:UIAlertActionStyleDefault
                                          handler:^(UIAlertAction *action) {
        (void)action;
        [weakSelf presentPickerForImport:GP32ImportBIOS];
    }]];
    [menu addAction:[UIAlertAction actionWithTitle:(_emulator.hasGame ? @"Reload game" : @"Run game")
                                            style:UIAlertActionStyleDefault
                                          handler:^(UIAlertAction *action) {
        (void)action;
        typeof(self) self = weakSelf;
        if (!self) return;
        if (self->_gamePath) {
            [self loadContentAtPath:self->_gamePath];
            [self menuDidClose];
        } else {
            [self presentPickerForImport:GP32ImportGame];
        }
    }]];

    NSString *bootTitle = @"Boot mode: Auto";
    if (_bootMode == GP32_IOS_BOOT_BIOS) bootTitle = @"Boot mode: BIOS";
    else if (_bootMode == GP32_IOS_BOOT_DIRECT) bootTitle = @"Boot mode: Direct";
    [menu addAction:[UIAlertAction actionWithTitle:bootTitle
                                            style:UIAlertActionStyleDefault
                                          handler:^(UIAlertAction *action) {
        (void)action;
        typeof(self) self = weakSelf;
        if (!self) return;
        UIAlertController *chooser =
            [UIAlertController alertControllerWithTitle:@"Boot mode"
                                                message:@"Auto boots through the BIOS when one is imported; Direct extracts the executable from the card."
                                         preferredStyle:UIAlertControllerStyleActionSheet];
        NSArray<NSString *> *titles = @[@"Auto", @"BIOS", @"Direct"];
        for (NSInteger index = 0; index < (NSInteger)titles.count; ++index) {
            [chooser addAction:[UIAlertAction actionWithTitle:titles[(NSUInteger)index]
                                                       style:UIAlertActionStyleDefault
                                                     handler:^(UIAlertAction *choice) {
                (void)choice;
                typeof(self) self2 = weakSelf;
                if (!self2) return;
                [self2 chooseBootMode:(gp32_ios_boot_mode_t)index];
                [self2 menuDidClose];
            }]];
        }
        [chooser addAction:[UIAlertAction actionWithTitle:@"Cancel"
                                                    style:UIAlertActionStyleCancel
                                                  handler:^(UIAlertAction *choice) {
            (void)choice;
            typeof(self) self2 = weakSelf;
            if (self2) [self2 menuDidClose];
        }]];
        UIPopoverPresentationController *popover = chooser.popoverPresentationController;
        popover.sourceView = self.view;
        popover.sourceRect = CGRectMake(CGRectGetMidX(self.view.bounds),
                                       CGRectGetMidY(self.view.bounds), 1, 1);
        popover.permittedArrowDirections = 0;
        chooser.presentationController.delegate = self;
        [self presentSoon:chooser];
    }]];

    [menu addAction:[UIAlertAction actionWithTitle:@"Save state"
                                            style:UIAlertActionStyleDefault
                                          handler:^(UIAlertAction *action) {
        (void)action;
        typeof(self) self = weakSelf;
        if (!self) return;
        [self->_emulator saveState:nil];
        [self menuDidClose];
    }]];
    [menu addAction:[UIAlertAction actionWithTitle:@"Load state"
                                            style:UIAlertActionStyleDefault
                                          handler:^(UIAlertAction *action) {
        (void)action;
        typeof(self) self = weakSelf;
        if (!self) return;
        [self->_emulator loadState:nil];
        [self menuDidClose];
    }]];
    [menu addAction:[UIAlertAction actionWithTitle:@"Flush card save"
                                            style:UIAlertActionStyleDefault
                                          handler:^(UIAlertAction *action) {
        (void)action;
        typeof(self) self = weakSelf;
        if (!self) return;
        [self->_emulator saveCardProgress:nil];
        [self menuDidClose];
    }]];
    [menu addAction:[UIAlertAction actionWithTitle:@"Reset console"
                                            style:UIAlertActionStyleDefault
                                          handler:^(UIAlertAction *action) {
        (void)action;
        typeof(self) self = weakSelf;
        if (!self) return;
        [self->_emulator reset];
        [self menuDidClose];
    }]];
    [menu addAction:[UIAlertAction actionWithTitle:(_userPaused ? @"Resume" : @"Pause")
                                            style:UIAlertActionStyleDefault
                                          handler:^(UIAlertAction *action) {
        (void)action;
        typeof(self) self = weakSelf;
        if (!self) return;
        self->_userPaused = !self->_userPaused;
        if (!self->_userPaused) self->_lastTickTime = 0.0;
        [self menuDidClose];
    }]];
    [menu addAction:[UIAlertAction actionWithTitle:@"Close"
                                            style:UIAlertActionStyleCancel
                                          handler:^(UIAlertAction *action) {
        (void)action;
        typeof(self) self = weakSelf;
        if (self) [self menuDidClose];
    }]];

    /* Anchored to the middle of the root view, so no control area is reused as
     * the menu's hit region. */
    UIPopoverPresentationController *popover = menu.popoverPresentationController;
    popover.sourceView = self.view;
    popover.sourceRect = CGRectMake(CGRectGetMidX(self.view.bounds),
                                    CGRectGetMidY(self.view.bounds), 1.0, 1.0);
    popover.permittedArrowDirections = 0;

    menu.presentationController.delegate = self;
    [self presentViewController:menu animated:YES completion:nil];
}

/* Ends the menu session and resumes pacing and the guest when the app should
 * run. Every action handler and the document picker completion call this,
 * because tapping an action dismisses the sheet without reporting through
 * presentationControllerDidDismiss: (that callback only covers a swipe or a
 * tap outside). The guard keeps repeated calls idempotent. */
- (void)menuDidClose {
    if (!_menuOpen) return;
    _menuOpen = NO;
    [self revealMenuButton];
    _lastTickTime = 0.0;
    _displayLink.paused = !_foreground;
    [self applyRunningState];
}

- (void)presentationControllerDidDismiss:(UIPresentationController *)presentationController {
    (void)presentationController;
    [self menuDidClose];
}

#pragma mark - Focus and interruption

#if GP32EMU_IOS_DEVELOPER_JIT
- (void)finishExternalJIT:(NSString *)message {
    ++_jitRequest; /* Invalidates a late URL callback or owner completion. */
    [_jitWaitTimer invalidate];
    _jitWaitTimer = nil;
    _jitPending = NO;
    _jitTransitioning = NO;
    if (_jitBackgroundTask != UIBackgroundTaskInvalid) {
        [UIApplication.sharedApplication endBackgroundTask:_jitBackgroundTask];
        _jitBackgroundTask = UIBackgroundTaskInvalid;
    }
    _lastTickTime = 0;
    [self applyRunningState];
    [self showStatus:message temporary:_emulator.jitActive];
}

- (void)requestExternalJIT {
    if (_jitPending || !_emulator.hasGame || _emulator.jitActive) return;
    NSString *error = GP32JITExternalError();
    if (!error && GP32JITDebuggerAttached())
        error = @"Disconnect the current debugger before requesting StikDebug.";
    NSURL *url = GP32JITExternalURL();
    if (!error && (!url || ![UIApplication.sharedApplication canOpenURL:url]))
        error = @"Install and set up StikDebug first, then choose Enable JIT again. The game can run with the interpreter.";
    if (error) { [self showStatus:error temporary:NO]; return; }

    _jitPending = YES;
    NSUInteger request = ++_jitRequest;
    _jitDeadline = CACurrentMediaTime() + 30.0;
    [self applyRunningState];
    __weak typeof(self) weakSelf = self;
    _jitBackgroundTask = [UIApplication.sharedApplication beginBackgroundTaskWithName:@"Prepare JIT"
        expirationHandler:^{
            dispatch_async(dispatch_get_main_queue(), ^{
                typeof(self) self = weakSelf;
                if (self && request == self->_jitRequest)
                    [self finishExternalJIT:@"JIT setup expired. Interpreter retained; try again when StikDebug is ready."];
            });
        }];
    if (_jitBackgroundTask == UIBackgroundTaskInvalid) {
        [self finishExternalJIT:@"iOS could not keep JIT setup running. Interpreter retained."];
        return;
    }
    _jitWaitTimer = [NSTimer timerWithTimeInterval:0.1 repeats:YES block:^(NSTimer *timer) {
        (void)timer;
        [weakSelf pollExternalJIT];
    }];
    [[NSRunLoop mainRunLoop] addTimer:_jitWaitTimer forMode:NSRunLoopCommonModes];
    [self showStatus:@"Waiting for StikDebug…" temporary:NO];
    [UIApplication.sharedApplication openURL:url options:@{} completionHandler:^(BOOL opened) {
        dispatch_async(dispatch_get_main_queue(), ^{
            typeof(self) self = weakSelf;
            if (self && request == self->_jitRequest && !opened)
                [self finishExternalJIT:@"Could not open StikDebug. Interpreter retained."];
        });
    }];
}

- (void)pollExternalJIT {
    if (!_jitPending || _jitTransitioning) return;
    if (CACurrentMediaTime() >= _jitDeadline) {
        [self finishExternalJIT:@"JIT setup timed out. Interpreter retained; check StikDebug and try again."];
        return;
    }
    /* This is a current attachment and follows an explicit universal.js
     * request. A persistent CS_DEBUGGED flag from an old session is not enough. */
    if (!GP32JITDebuggerAttached()) return;
    _jitTransitioning = YES;
    [_jitWaitTimer invalidate];
    _jitWaitTimer = nil;
    NSUInteger request = _jitRequest;
    __weak typeof(self) weakSelf = self;
    [_emulator activateJITWithUniversalScript:^(BOOL success, NSString *message) {
        (void)success;
        typeof(self) self = weakSelf;
        if (self && request == self->_jitRequest) [self finishExternalJIT:message];
    }];
}
#endif

- (void)handleAudioSystemNotification:(NSNotification *)note {
    /* AVAudioEngine posts on an internal queue. Never tear it down from that
     * callback or synchronously wait on the main queue: teardown can deadlock. */
    __weak typeof(self) weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        typeof(self) self = weakSelf;
        if (!self) return;
        if ([note.name isEqualToString:AVAudioSessionInterruptionNotification]) {
            [self handleAudioInterruption:note];
        } else if ([note.name isEqualToString:AVAudioEngineConfigurationChangeNotification]) {
            if (![self->_audio invalidateAfterConfigurationChange:note]) return;
            self->_audioRunning = NO;
            self->_lastTickTime = 0.0;
            [self applyRunningState];
        } else if ([note.name isEqualToString:AVAudioSessionRouteChangeNotification]) {
            NSNumber *reason = note.userInfo[AVAudioSessionRouteChangeReasonKey];
            if (reason.unsignedIntegerValue != AVAudioSessionRouteChangeReasonOldDeviceUnavailable) return;
            self->_userPaused = YES;
            [self applyRunningState];
            [self showStatus:@"Audio device disconnected. Resume from the menu." temporary:NO];
        } else if ([note.name isEqualToString:AVAudioSessionMediaServicesWereResetNotification]) {
            /* The old audio objects are unusable. Apple requires user action
             * before reactivating the session after a media-server reset. */
            self->_userPaused = YES;
            self->_audioInterrupted = NO;
            [self->_audio resetAfterMediaServicesReset];
            self->_audioRunning = NO;
            self->_lastTickTime = 0.0;
            [self applyRunningState];
            [self showStatus:@"Audio was reset. Resume from the menu." temporary:NO];
        }
    });
}

- (void)applicationDidBecomeActive:(NSNotification *)note {
    (void)note;
    _foreground = YES;
    _lastTickTime = 0.0;
    _displayLink.paused = NO;
    [self applyRunningState];
}

- (void)applicationWillResignActive:(NSNotification *)note {
    (void)note;
    [self pauseForBackground];
}

- (void)applicationDidEnterBackground:(NSNotification *)note {
    (void)note;
    _foreground = NO;
    _displayLink.paused = YES;
}

/* Lifecycle pause: stop pacing, release every held button, stop and flush audio
 * and persist pending card progress before the system suspends the process. */
- (void)pauseForBackground {
    _foreground = NO;
    _lastTickTime = 0.0;
    _displayLink.paused = YES;
    [_emulator pause];
    [_emulator releaseInput];
    [self clearInputState];
    if (_audioRunning) {
        [_audio stop];
        _audioRunning = NO;
    }
    if (_emulator.hasGame) [_emulator saveCardProgress:nil];
}

- (void)handleAudioInterruption:(NSNotification *)note {
    NSNumber *type = note.userInfo[AVAudioSessionInterruptionTypeKey];
    if (!type) return;
    if (type.unsignedIntegerValue == AVAudioSessionInterruptionTypeBegan) {
        _audioInterrupted = YES;
        [_emulator pause];
        [_emulator releaseInput];
        [self clearInputState];
        /* The system owns the session during an interruption. Stopping the
         * graph and flushing the ring is what keeps the resume from replaying
         * PCM the guest queued before it. */
        [_audio pauseForInterruption];
        _audioRunning = NO;
        return;
    }

    _audioInterrupted = NO;
    NSNumber *options = note.userInfo[AVAudioSessionInterruptionOptionKey];
    if (options.unsignedIntegerValue & AVAudioSessionInterruptionOptionShouldResume) {
        _lastTickTime = 0.0;
        /* startWithError: flushes the ring and reconfigures the session, so the
         * resume path clears inputs, pacing and stale audio. */
        [self applyRunningState];
    } else {
        _userPaused = YES;
        [self showStatus:@"Paused after an audio interruption. Resume from the menu." temporary:NO];
    }
}

@end
