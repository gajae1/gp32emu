/*
 * GP32Emulator: the only object in the app that calls the C core.
 *
 * Concurrency contract
 *   - Every gp32_ios_emulator_* call runs on one serial owner queue, so two
 *     core calls can never overlap and the main thread never enters the core.
 *   - -tick is main-thread only and starts at most one frame at a time. Ticks
 *     that arrive while a frame is in flight retain at most one owed interval.
 *     Completion can catch that interval up without a growing backlog; pause
 *     or content replacement clears it.
 *   - The frame handler receives an immutable NSData snapshot taken on the
 *     owner queue before the UI dispatch, so later reset/load/persistence work
 *     on the owner queue can never mutate pixels the UI is about to read.
 *   - Each load request takes a generation token. A newer request makes an
 *     older, unfinished one a no-op, including its completion.
 *   - The audio sink is a fixed-size lock-free ring owned by GP32Audio. The
 *     wrapper retains that owner: a tick or load block can still be queued on
 *     the owner queue after the view controller releases its references, so
 *     the ring must outlive the drained queue and the destroyed core. The
 *     wrapper itself never reads or writes the ring.
 */

#import <Foundation/Foundation.h>

#include "gp32_ios.h"

@class GP32Audio;

NS_ASSUME_NONNULL_BEGIN

/* Called on the main thread with an immutable snapshot of GP32_IOS_FRAME_PIXELS
 * 0xAARRGGBB words. Retaining the data keeps the pixels valid. */
typedef void (^GP32FrameHandler)(NSData *pixels, uint64_t frameCounter);

/* Called on the main thread. */
typedef void (^GP32StatusHandler)(NSString *message, BOOL temporary);
typedef void (^GP32ResultHandler)(BOOL success, NSString *message);

@interface GP32Emulator : NSObject

/* Retains `audio` for the lifetime of this object. -dealloc drains the owner
 * queue and destroys the core before ARC releases that reference, so the sink
 * the core writes into is never freed under a queued or in-flight call. */
- (instancetype)initWithAudio:(GP32Audio *)audio NS_DESIGNATED_INITIALIZER;
- (instancetype)init NS_UNAVAILABLE;

@property (nonatomic, copy, nullable) GP32FrameHandler frameHandler;
@property (nonatomic, copy, nullable) GP32StatusHandler statusHandler;

@property (nonatomic, readonly) BOOL hasGame;
@property (nonatomic, readonly, getter=isPaused) BOOL paused;
@property (nonatomic, readonly, copy, nullable) NSString *contentLabel;
@property (nonatomic, readonly, copy, nullable) NSString *contentPath;

/* Replaces whatever was loaded and persists the previous card first. A failed
 * save preserves the old session, paused, and reports failure. A request
 * issued while an earlier one is still loading supersedes it; the superseded
 * completion is not called. */
- (void)loadContentAtPath:(NSString *)gamePath
                 biosPath:(nullable NSString *)biosPath
                    label:(nullable NSString *)label
             cardSavePath:(nullable NSString *)cardSavePath
                statePath:(nullable NSString *)statePath
                 bootMode:(gp32_ios_boot_mode_t)bootMode
               completion:(nullable GP32ResultHandler)completion;

/* Frame delivery. `start` begins ticking, `pause` stops it and hands the guest
 * an all-buttons-up state so a touch that never ended cannot stay pressed. */
- (void)start;
- (void)pause;
- (void)tick;

- (void)setButtonMask:(uint32_t)mask;
- (void)releaseInput;
- (void)flushAudio;
- (void)reset;

#if GP32EMU_IOS_DEVELOPER_JIT
/* Developer-tool entry points. Call on main, wait for the asynchronous owner
 * completion before touching the region. Never use a region after content
 * changes. No debugger grant is inferred from allocation or app signing. */
@property (nonatomic, readonly, copy, nullable) NSDictionary *jitPreparation;
/* Main-thread snapshot published after owner-queue transitions. */
@property (nonatomic, readonly) BOOL jitActive;
- (void)prepareJIT;
- (void)activateJIT;
/* Called only with the matching universal.js debugger attached. */
- (void)activateJITWithUniversalScript:(nullable GP32ResultHandler)completion;
#endif

/* Card progress is also saved automatically by the running core; these are the
 * explicit flush and state-file operations the menu drives. The legacy state
 * methods use the path supplied when the content loaded. The explicit-path
 * variants validate and use that path on the owner queue without changing the
 * configured legacy path. Messages are the core's own error text. */
- (void)saveCardProgress:(nullable GP32ResultHandler)completion;
- (void)saveState:(nullable GP32ResultHandler)completion;
- (void)loadState:(nullable GP32ResultHandler)completion;
- (void)saveStateAtPath:(NSString *)statePath
             completion:(nullable GP32ResultHandler)completion;
- (void)loadStateAtPath:(NSString *)statePath
             completion:(nullable GP32ResultHandler)completion;

@end

NS_ASSUME_NONNULL_END
