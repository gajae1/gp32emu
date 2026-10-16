/*
 * GP32Audio: stereo playback of the emulator stream through AVAudioEngine.
 *
 * The guest rate is resampled to GP32_IOS_AUDIO_RATE by the C bridge into a
 * bounded s16 ring. The source node is created with an explicit stereo Float32
 * non-interleaved format, so the render block knows exactly which buffer is
 * which channel and converts s16 to float itself. It allocates nothing, takes
 * no lock, dispatches nothing and never calls the core. If the node is ever
 * handed a different layout the block writes silence and counts it in
 * sink->layout_mismatch_events instead of guessing at the samples.
 *
 * The interruption policy (pause the emulator, release input, resume) lives in
 * GP32EmulatorViewController; this class only starts, pauses for an interruption
 * and stops the engine/session when asked. The ring outlives every callback
 * because the source node is detached before the ring is freed.
 */

#import <Foundation/Foundation.h>

#include "gp32_ios.h"

NS_ASSUME_NONNULL_BEGIN

@interface GP32Audio : NSObject

/* Stable for the lifetime of this object; hand it to GP32Emulator. */
@property (nonatomic, readonly) gp32_ios_audio_sink_t *sink;

/* YES only while playback is requested and the engine is actually running. */
@property (nonatomic, readonly, getter=isRunning) BOOL running;

/* Frames the device asked for and the ring could not supply. */
@property (nonatomic, readonly) uint32_t underrunEvents;

/* Render callbacks that did not receive the requested mono-per-channel stereo
 * Float32 layout; those callbacks stayed silent. */
@property (nonatomic, readonly) uint32_t formatMismatchEvents;

/* Starting a stopped engine discards queued audio and configures the session.
 * An already running engine is left uninterrupted. If startup fails, attempts
 * to deactivate the session activated for that start. */
- (BOOL)startWithError:(NSError **)error;

/* Stops the engine for an AVAudioSession interruption. The system already owns
 * the session at that point, so nothing here reactivates it. */
- (void)pauseForInterruption;

/* Main-thread only, after leaving the engine's notification callback. Returns
 * NO for a stale notification from an engine this object no longer owns. */
- (BOOL)invalidateAfterConfigurationChange:(NSNotification *)notification;

/* Discard objects invalidated by a media-server reset. Rebuilt on next start;
 * the controller requires an explicit user resume before activating audio. */
- (void)resetAfterMediaServicesReset;

/* Stops the engine, discards queued audio and deactivates the session. */
- (void)stop;

@end

NS_ASSUME_NONNULL_END
