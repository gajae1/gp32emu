#import "GP32Emulator.h"

#import "GP32Audio.h"
#import "GP32JIT.h"

#include <stdatomic.h>
#include <string.h>

/* Queue identity used to keep -dealloc from deadlocking if it ever runs on the
 * owner queue instead of the main thread. */
static const void *GP32EmulatorQueueKey = &GP32EmulatorQueueKey;

@interface GP32Emulator ()
@property (nonatomic, readwrite) BOOL hasGame;
@property (nonatomic, readwrite, getter=isPaused) BOOL paused;
@property (nonatomic, readwrite, copy, nullable) NSString *contentLabel;
@property (nonatomic, readwrite, copy, nullable) NSString *contentPath;
#if GP32EMU_IOS_DEVELOPER_JIT
@property (nonatomic, readwrite, copy, nullable) NSDictionary *jitPreparation;
@property (nonatomic, readwrite) BOOL jitActive;
#endif
@end

@implementation GP32Emulator {
    gp32_ios_emulator_t *_emulator;
    GP32Audio *_audioOwner;       /* Retained: the sink must outlive the queue. */
    gp32_ios_audio_sink_t *_sink; /* Borrowed from _audioOwner for the core. */
    dispatch_queue_t _ownerQueue;
    dispatch_source_t _frameTimer;
    _Atomic BOOL _clockRunning;
    _Atomic NSUInteger _runToken;
    NSUInteger _timerRunToken; /* Owner queue: generation which armed the timer. */
    /* Protected by @synchronized(self). Presentation never gates execution. */
    NSData *_pendingFrame;
    BOOL _presentationQueued;
    uint64_t _pendingFrameCounter;
    NSUInteger _pendingFrameLoadToken, _pendingFrameRunToken;
    _Atomic NSUInteger _loadToken; /* Main thread bumps; owner queue observes. */
    NSString *_loadedPath;  /* Owner-queue identity, including superseded loads. */
    NSString *_loadedLabel;
}

- (instancetype)initWithAudio:(GP32Audio *)audio {
    self = [super init];
    if (!self) return nil;

    _emulator = gp32_ios_emulator_create();
    if (!_emulator) return nil;

    _audioOwner = audio;
    _sink = audio.sink;
    _ownerQueue = dispatch_queue_create("org.gp32emu.ios.emulator", DISPATCH_QUEUE_SERIAL);
    dispatch_queue_set_specific(_ownerQueue, GP32EmulatorQueueKey, (void *)GP32EmulatorQueueKey, NULL);
    _frameTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, _ownerQueue);
    dispatch_source_set_timer(_frameTimer, DISPATCH_TIME_FOREVER, DISPATCH_TIME_FOREVER, 0);
    __weak typeof(self) weakSelf = self;
    dispatch_source_set_event_handler(_frameTimer, ^{ [weakSelf runScheduledFrames]; });
    dispatch_resume(_frameTimer);
    _paused = YES;
    return self;
}

- (void)dealloc {
    if (_frameTimer) dispatch_source_cancel(_frameTimer);
    /* The sync drains every block already queued on the owner queue, then
     * releases the core on that same queue, so every gp32_* call keeps the
     * single-owner rule. Only after this body returns does ARC release
     * _audioOwner, so the sink the core writes into stays allocated for the
     * whole drain. */
    gp32_ios_emulator_t *emulator = _emulator;
    if (!emulator) return; /* init may fail before creating the owner queue. */
    if (!_ownerQueue || dispatch_get_specific(GP32EmulatorQueueKey) == GP32EmulatorQueueKey) {
        gp32_ios_emulator_destroy(emulator);
    } else {
        dispatch_sync(_ownerQueue, ^{ gp32_ios_emulator_destroy(emulator); });
    }
}

#pragma mark - Status

- (void)publishStatus:(NSString *)message {
    [self publishStatus:message temporary:NO];
}

- (void)publishStatus:(NSString *)message temporary:(BOOL)temporary {
    GP32StatusHandler handler = self.statusHandler;
    if (handler) handler(message ?: @"", temporary);
}

- (NSString *)coreError {
    const char *text = gp32_ios_emulator_error(_emulator);
    return text ? [NSString stringWithUTF8String:text] : @"";
}

#pragma mark - Content

- (void)loadContentAtPath:(NSString *)gamePath
                 biosPath:(NSString *)biosPath
                    label:(NSString *)label
             cardSavePath:(NSString *)cardSavePath
                statePath:(NSString *)statePath
                 bootMode:(gp32_ios_boot_mode_t)bootMode
               completion:(GP32ResultHandler)completion {
    if (!gamePath.length) {
        [self publishStatus:@"No game image is selected."];
        if (completion) completion(NO, @"No game image is selected.");
        return;
    }

    /* Single-load gate: a newer request makes this one a no-op, so a slow
     * earlier completion can never mark a later selection as loaded. */
    NSUInteger token = atomic_fetch_add_explicit(&_loadToken, 1u, memory_order_acq_rel) + 1u;

    /* Invalidate the owner clock before queuing the content swap. Pending
     * timer events cannot execute the superseded generation. */
    self.paused = YES;
    [self stopClock];
    self.hasGame = NO;
    self.contentLabel = nil;
#if GP32EMU_IOS_DEVELOPER_JIT
    self.jitPreparation = nil;
    self.jitActive = NO;
#endif
    [self publishStatus:@"Loading…"];

    NSString *game = [gamePath copy];
    NSString *bios = [biosPath copy];
    NSString *name = [label copy];
    NSString *card = [cardSavePath copy];
    NSString *state = [statePath copy];
    NSString *fallbackName = [game.lastPathComponent copy];

    dispatch_async(_ownerQueue, ^{
        if (token != atomic_load_explicit(&self->_loadToken, memory_order_acquire)) {
            return; /* A newer selection already owns the load. */
        }
        gp32_ios_emulator_t *emulator = self->_emulator;
        /* Never discard the only copy of progress after a failed save. */
        int released = gp32_ios_emulator_unload(emulator);
        if (released) {
            self->_loadedPath = nil;
            self->_loadedLabel = nil;
        }

        gp32_ios_config_t config;
        memset(&config, 0, sizeof(config));
        config.bios_path = bios.length ? bios.fileSystemRepresentation : NULL;
        config.game_path = game.fileSystemRepresentation;
        config.game_label = name.length ? name.fileSystemRepresentation : NULL;
        config.card_save_path = card.length ? card.fileSystemRepresentation : NULL;
        config.state_path = state.length ? state.fileSystemRepresentation : NULL;
        config.audio_sink = self->_sink;
        config.boot_mode = bootMode;

        int loaded = released && gp32_ios_emulator_load(emulator, &config) != 0;
        NSString *message = loaded ? nil : [self coreError];
        if (!released) {
            message = [NSString stringWithFormat:@"Could not save progress. The current game was kept paused. %@", message ?: @""];
        }
        if (loaded) {
            self->_loadedPath = game;
            self->_loadedLabel = name.length ? name : fallbackName;
        }
        BOOL hasGame = gp32_ios_emulator_has_game(emulator) != 0;
        NSString *actualPath = self->_loadedPath;
        NSString *actualLabel = self->_loadedLabel;
#if GP32EMU_IOS_DEVELOPER_JIT
        BOOL jitActive = gp32_ios_emulator_jit_enabled(emulator) != 0;
#endif

        dispatch_async(dispatch_get_main_queue(), ^{
            if (token != atomic_load_explicit(&self->_loadToken, memory_order_acquire)) {
                return; /* Superseded while the core was loading. */
            }
            self.hasGame = hasGame;
            self.contentPath = actualPath;
            self.contentLabel = actualLabel;
#if GP32EMU_IOS_DEVELOPER_JIT
            self.jitActive = jitActive;
#endif
            self.paused = !loaded;
            if (loaded) {
                [self publishStatus:[NSString stringWithFormat:@"Running %@", self.contentLabel ?: fallbackName]];
            } else {
                [self publishStatus:message.length ? message : @"The content could not be loaded."];
            }
            if (completion) completion(loaded, loaded ? (message ?: @"") : (message.length ? message : @"The content could not be loaded."));
        });
    });
}

#pragma mark - Execution

#if GP32EMU_IOS_DEVELOPER_JIT
- (void)activateJITWithUniversalScript:(GP32ResultHandler)completion {
    self.jitPreparation = nil;
    NSUInteger token = atomic_load_explicit(&_loadToken, memory_order_acquire);
    dispatch_async(_ownerQueue, ^{
        BOOL current = token == atomic_load_explicit(&self->_loadToken, memory_order_acquire);
        BOOL ok = current && GP32JITEnableWithUniversalScript(self->_emulator);
        dispatch_async(dispatch_get_main_queue(), ^{
            if (token != atomic_load_explicit(&self->_loadToken, memory_order_acquire)) {
                if (completion) completion(NO, @"Content changed during JIT setup.");
                return;
            }
            self.jitActive = ok;
            NSString *message = ok ? @"JIT enabled." : @"JIT activation refused; interpreter retained.";
            [self publishStatus:message temporary:YES];
            if (completion) completion(ok, message);
        });
    });
}

- (void)prepareJIT {
    self.jitPreparation = nil;
    NSUInteger token = atomic_load_explicit(&_loadToken, memory_order_acquire);
    dispatch_async(_ownerQueue, ^{
        if (token != atomic_load_explicit(&self->_loadToken, memory_order_acquire)) return;
        gp32_ios_jit_region_t region;
        BOOL active = gp32_ios_emulator_jit_enabled(self->_emulator) != 0;
        int ok = !active && gp32_ios_emulator_prepare_jit(self->_emulator, &region);
        NSDictionary *result = ok ? @{@"address": @((uintptr_t)region.execute),
            @"size": @(region.size), @"pageSize": @(region.page_size),
            @"marker": @(region.marker), @"generation": @(token)} : nil;
        dispatch_async(dispatch_get_main_queue(), ^{
            if (token != atomic_load_explicit(&self->_loadToken, memory_order_acquire)) return;
            self.jitPreparation = result;
            self.jitActive = active;
            [self publishStatus:active ? @"JIT is already enabled."
                              : ok ? @"JIT memory prepared. Waiting for debugger initialization."
                                   : @"JIT preparation unavailable; interpreter retained." temporary:YES];
        });
    });
}

- (void)activateJIT {
    NSDictionary *region = self.jitPreparation;
    if (!region) return;
    NSUInteger token = [region[@"generation"] unsignedIntegerValue];
    dispatch_async(_ownerQueue, ^{
        if (token != atomic_load_explicit(&self->_loadToken, memory_order_acquire)) return;
        (void)gp32_ios_emulator_activate_jit(self->_emulator);
        BOOL active = gp32_ios_emulator_jit_enabled(self->_emulator) != 0;
        dispatch_async(dispatch_get_main_queue(), ^{
            if (token != atomic_load_explicit(&self->_loadToken, memory_order_acquire)) return;
            /* A failed transition can release the prepared mapping. Never
             * expose a stale address; a retry starts with prepare again. */
            self.jitPreparation = nil;
            self.jitActive = active;
            [self publishStatus:active ? @"JIT enabled." : @"JIT activation refused; interpreter retained." temporary:YES];
        });
    });
}
#endif

- (void)start {
    if (!self.hasGame) {
        [self publishStatus:@"Load a BIOS or game image first."];
        return;
    }
    self.paused = NO;
    if (!atomic_load_explicit(&_clockRunning, memory_order_acquire)) {
        NSUInteger run = atomic_fetch_add_explicit(&_runToken, 1u, memory_order_acq_rel) + 1u;
        atomic_store_explicit(&_clockRunning, YES, memory_order_release);
        dispatch_async(_ownerQueue, ^{
            if (run != atomic_load_explicit(&self->_runToken, memory_order_acquire) ||
                !atomic_load_explicit(&self->_clockRunning, memory_order_acquire)) return;
            self->_timerRunToken = run;
            dispatch_source_set_timer(self->_frameTimer, DISPATCH_TIME_NOW, NSEC_PER_SEC / 60,
                                      NSEC_PER_MSEC / 2);
        });
    }
    [self publishStatus:[NSString stringWithFormat:@"Running %@", self.contentLabel ?: @"content"]];
}

- (void)pause {
    BOOL wasRunning = !self.paused;
    self.paused = YES;
    [self stopClock];
    [self releaseInput];
    /* A lifecycle pause must not hide the initial import hint or a load error. */
    if (wasRunning) [self publishStatus:@"Paused."];
}

- (void)stopClock {
    atomic_store_explicit(&_clockRunning, NO, memory_order_release);
    atomic_fetch_add_explicit(&_runToken, 1u, memory_order_acq_rel);
    dispatch_async(_ownerQueue, ^{
        dispatch_source_set_timer(self->_frameTimer, DISPATCH_TIME_FOREVER, DISPATCH_TIME_FOREVER, 0);
    });
    @synchronized(self) { _pendingFrame = nil; }
}

/* The serial owner queue advances guest time independently of display refresh
 * and main-thread work. Missed timer events can owe one extra interval, as
 * before; a long suspension cannot create an unbounded fast-forward burst. */
- (void)runScheduledFrames {
    if (!atomic_load_explicit(&_clockRunning, memory_order_acquire)) return;
    NSUInteger token = atomic_load_explicit(&_loadToken, memory_order_acquire);
    NSUInteger run = atomic_load_explicit(&_runToken, memory_order_acquire);
    if (run != _timerRunToken) return;
    unsigned long due = dispatch_source_get_data(_frameTimer);
    if (due > 2) due = 2;
    for (unsigned long i = 0; i < due; ++i) {
        if (!atomic_load_explicit(&_clockRunning, memory_order_acquire) ||
            run != atomic_load_explicit(&_runToken, memory_order_acquire) ||
            token != atomic_load_explicit(&_loadToken, memory_order_acquire)) return;
        gp32_ios_emulator_t *emulator = _emulator;
        int ran = gp32_ios_emulator_run_frame(emulator) != 0;
        if (!ran) {
            atomic_store_explicit(&_clockRunning, NO, memory_order_release);
            NSString *message = [self coreError];
            dispatch_async(dispatch_get_main_queue(), ^{
                if (token != atomic_load_explicit(&self->_loadToken, memory_order_acquire) ||
                    run != atomic_load_explicit(&self->_runToken, memory_order_acquire)) return;
                [self pause];
                [self publishStatus:message.length ? message : @"The emulated CPU stopped."];
            });
            return;
        }
        const uint32_t *pixels = gp32_ios_emulator_frame_pixels(emulator);
        NSData *snapshot = pixels ? [[NSData alloc] initWithBytes:pixels
                                     length:GP32_IOS_FRAME_PIXELS * sizeof(uint32_t)] : nil;
        uint64_t counter = gp32_ios_emulator_frame_counter(emulator);
        /* Keep only the newest completed image while UIKit is busy. The core
         * and PCM still advance; no mutable core memory reaches the UI. */
        BOOL notify;
        @synchronized(self) {
            _pendingFrame = snapshot;
            _pendingFrameCounter = counter;
            _pendingFrameLoadToken = token;
            _pendingFrameRunToken = run;
            notify = !_presentationQueued;
            _presentationQueued = YES;
        }
        if (notify) {
            dispatch_async(dispatch_get_main_queue(), ^{
                @synchronized(self) { self->_presentationQueued = NO; }
                [self tick];
            });
        }
    }
}

- (void)tick {
    if (self.paused || !self.hasGame) return;
    NSData *snapshot;
    uint64_t counter;
    NSUInteger token, run;
    @synchronized(self) {
        snapshot = _pendingFrame;
        _pendingFrame = nil;
        counter = _pendingFrameCounter;
        token = _pendingFrameLoadToken;
        run = _pendingFrameRunToken;
    }
    if (snapshot && token == atomic_load_explicit(&_loadToken, memory_order_acquire) &&
        run == atomic_load_explicit(&_runToken, memory_order_acquire)) {
        GP32FrameHandler handler = self.frameHandler;
        if (handler) handler(snapshot, counter);
    }
}

- (void)setButtonMask:(uint32_t)mask {
    dispatch_async(_ownerQueue, ^{
        gp32_ios_emulator_set_buttons(self->_emulator, mask);
    });
}

- (void)releaseInput {
    dispatch_async(_ownerQueue, ^{
        gp32_ios_emulator_release_input(self->_emulator);
    });
}

- (void)flushAudio {
    dispatch_async(_ownerQueue, ^{
        gp32_ios_emulator_clear_audio(self->_emulator);
    });
}

- (void)reset {
    dispatch_async(_ownerQueue, ^{
        int ok = gp32_ios_emulator_reset(self->_emulator) != 0;
        NSString *message = ok ? @"Console reset." : [self coreError];
        dispatch_async(dispatch_get_main_queue(), ^{
            [self publishStatus:message.length ? message : @"The console could not be reset." temporary:ok];
        });
    });
}

#pragma mark - Persistence

- (void)runPersistenceWithSuccess:(NSString *)successMessage
                        completion:(GP32ResultHandler)completion
                         operation:(int (^)(gp32_ios_emulator_t *emulator))operation {
    dispatch_async(_ownerQueue, ^{
        gp32_ios_emulator_t *emulator = self->_emulator;
        int ok = operation(emulator) != 0;
        NSString *message = ok ? successMessage : [self coreError];
        if (!message.length) message = @"The operation failed.";
        dispatch_async(dispatch_get_main_queue(), ^{
            [self publishStatus:message temporary:ok];
            if (completion) completion(ok, message);
        });
    });
}

- (void)saveCardProgress:(GP32ResultHandler)completion {
    [self runPersistenceWithSuccess:@"Card progress saved."
                         completion:completion
                          operation:^int(gp32_ios_emulator_t *emulator) {
                              return gp32_ios_emulator_save_card(emulator);
                          }];
}

- (void)saveState:(GP32ResultHandler)completion {
    [self runPersistenceWithSuccess:@"State saved."
                         completion:completion
                          operation:^int(gp32_ios_emulator_t *emulator) {
                              return gp32_ios_emulator_save_state(emulator);
                          }];
}

- (void)loadState:(GP32ResultHandler)completion {
    [self runPersistenceWithSuccess:@"State loaded."
                         completion:completion
                          operation:^int(gp32_ios_emulator_t *emulator) {
                              return gp32_ios_emulator_load_state(emulator);
                          }];
}

- (void)saveStateAtPath:(NSString *)statePath completion:(GP32ResultHandler)completion {
    NSString *state = [statePath copy];
    [self runPersistenceWithSuccess:@"State saved."
                         completion:completion
                          operation:^int(gp32_ios_emulator_t *emulator) {
                              return gp32_ios_emulator_save_state_at_path(
                                  emulator, state.fileSystemRepresentation);
                          }];
}

- (void)loadStateAtPath:(NSString *)statePath completion:(GP32ResultHandler)completion {
    NSString *state = [statePath copy];
    [self runPersistenceWithSuccess:@"State loaded."
                         completion:completion
                          operation:^int(gp32_ios_emulator_t *emulator) {
                              return gp32_ios_emulator_load_state_at_path(
                                  emulator, state.fileSystemRepresentation);
                          }];
}

@end
