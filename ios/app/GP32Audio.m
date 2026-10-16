#import "GP32Audio.h"

#import <AVFoundation/AVFoundation.h>

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* s16 frames converted per pass inside the render callback. The scratch lives
 * on the audio thread stack: no allocation, no lock, no dispatch and no core
 * call between the callback entry and its return. */
enum { GP32AudioChunkFrames = 1024 };

/* The device callback. The node was created with a stereo Float32
 * non-interleaved format, so buffer 0 is the left channel and buffer 1 the
 * right. The layout is verified rather than assumed: a mismatch stays silent
 * and is counted, never played as misplaced samples. */
static OSStatus GP32AudioRender(gp32_ios_audio_sink_t *sink,
                                BOOL *isSilence,
                                AVAudioFrameCount frameCount,
                                AudioBufferList *outputData) {
    float *left = NULL;
    float *right = NULL;
    if (outputData->mNumberBuffers == 2u &&
        outputData->mBuffers[0].mNumberChannels == 1u &&
        outputData->mBuffers[1].mNumberChannels == 1u &&
        outputData->mBuffers[0].mData && outputData->mBuffers[1].mData &&
        outputData->mBuffers[0].mDataByteSize >= frameCount * sizeof(float) &&
        outputData->mBuffers[1].mDataByteSize >= frameCount * sizeof(float)) {
        left = (float *)outputData->mBuffers[0].mData;
        right = (float *)outputData->mBuffers[1].mData;
    }

    if (!left || !right) {
        for (UInt32 index = 0; index < outputData->mNumberBuffers; ++index) {
            if (outputData->mBuffers[index].mData) {
                memset(outputData->mBuffers[index].mData, 0, outputData->mBuffers[index].mDataByteSize);
            }
        }
        atomic_fetch_add_explicit(&sink->layout_mismatch_events, 1u, memory_order_relaxed);
        if (isSilence) *isSilence = YES;
        return noErr;
    }

    size_t suppliedTotal = 0;
    AVAudioFrameCount done = 0;
    while (done < frameCount) {
        AVAudioFrameCount chunk = frameCount - done;
        if (chunk > (AVAudioFrameCount)GP32AudioChunkFrames) chunk = (AVAudioFrameCount)GP32AudioChunkFrames;
        int16_t scratch[GP32AudioChunkFrames * 2];
        /* Pads with silence on underrun, so every output frame is written. */
        size_t supplied = gp32_ios_audio_sink_read(sink, scratch, (size_t)chunk);
        suppliedTotal += supplied;
        for (AVAudioFrameCount i = 0; i < chunk; ++i) {
            left[done + i] = (float)scratch[(size_t)i * 2u] * (1.0f / 32768.0f);
            right[done + i] = (float)scratch[(size_t)i * 2u + 1u] * (1.0f / 32768.0f);
        }
        done += chunk;
    }
    if (isSilence) *isSilence = (suppliedTotal == 0u);
    return noErr;
}

@implementation GP32Audio {
    gp32_ios_audio_sink_t *_sink;
    AVAudioEngine *_engine;
    AVAudioSourceNode *_source;
    BOOL _running;
}

- (instancetype)init {
    self = [super init];
    if (!self) return nil;

    _sink = calloc(1, sizeof(gp32_ios_audio_sink_t));
    if (!_sink) return nil;
    /* No consumer can observe the sink yet: the one moment the shared indices
     * may be reset. Every later discard goes through the flush request. */
    gp32_ios_audio_sink_init(_sink);
    return self;
}

- (void)discardGraph {
    AVAudioEngine *engine = _engine;
    AVAudioSourceNode *source = _source;
    /* Retire identity first: delayed notifications from teardown must not
     * invalidate the replacement graph. The sink remains alive throughout. */
    _engine = nil;
    _source = nil;
    _running = NO;
    [engine stop];
    if (source) [engine detachNode:source];
    if (_sink) gp32_ios_audio_sink_request_flush(_sink);
}

- (void)dealloc {
    [self discardGraph];
    free(_sink);
}

- (BOOL)isRunning {
    return _running && _engine.isRunning;
}

- (BOOL)invalidateAfterConfigurationChange:(NSNotification *)notification {
    if (!_engine || notification.object != _engine) return NO;
    [self discardGraph];
    return YES;
}

- (void)resetAfterMediaServicesReset {
    [self discardGraph];
}

- (gp32_ios_audio_sink_t *)sink {
    return _sink;
}

- (uint32_t)underrunEvents {
    return _sink ? atomic_load_explicit(&_sink->underrun_events, memory_order_relaxed) : 0u;
}

- (uint32_t)formatMismatchEvents {
    return _sink ? atomic_load_explicit(&_sink->layout_mismatch_events, memory_order_relaxed) : 0u;
}

- (BOOL)buildGraphWithError:(NSError **)error {
    if (_engine) return YES;

    AVAudioFormat *format = [[AVAudioFormat alloc] initWithCommonFormat:AVAudioPCMFormatFloat32
                                                              sampleRate:(double)GP32_IOS_AUDIO_RATE
                                                                channels:2
                                                             interleaved:NO];
    gp32_ios_audio_sink_t *sink = _sink;
    AVAudioSourceNode *source = [[AVAudioSourceNode alloc] initWithFormat:format
        renderBlock:^OSStatus(BOOL *isSilence,
                              const AudioTimeStamp *timestamp,
                              AVAudioFrameCount frameCount,
                              AudioBufferList *outputData) {
            (void)timestamp;
            return GP32AudioRender(sink, isSilence, frameCount, outputData);
        }];
    if (!format || !source) {
        if (error) {
            *error = [NSError errorWithDomain:@"org.gp32emu.ios.audio"
                                        code:2
                                    userInfo:@{NSLocalizedDescriptionKey: @"The audio source node could not be created."}];
        }
        return NO;
    }

    AVAudioEngine *engine = [[AVAudioEngine alloc] init];
    [engine attachNode:source];
    [engine connect:source to:engine.mainMixerNode format:format];
    _engine = engine;
    _source = source;
    return YES;
}

- (BOOL)startWithError:(NSError **)error {
    if (!_sink) {
        if (error) {
            *error = [NSError errorWithDomain:@"org.gp32emu.ios.audio"
                                        code:1
                                    userInfo:@{NSLocalizedDescriptionKey: @"The audio ring could not be allocated."}];
        }
        return NO;
    }
    if (![self buildGraphWithError:error]) return NO;
    if (_engine.isRunning) {
        _running = YES;
        return YES;
    }

    /* Drop whatever was queued before this run (pause, interruption, state
     * restore): the consumer applies the discard at its next read. */
    gp32_ios_audio_sink_request_flush(_sink);

    AVAudioSession *session = AVAudioSession.sharedInstance;
    if (![session setCategory:AVAudioSessionCategoryPlayback
                        mode:AVAudioSessionModeDefault
                     options:0
                       error:error]) {
        _running = NO;
        return NO;
    }
    if (![session setActive:YES error:error]) {
        _running = NO;
        return NO;
    }
    if (![_engine startAndReturnError:error]) {
        /* The session was activated for this run but there is no playback to
         * justify it; release it so a failed start does not hold audio focus
         * until the next stop. The caller's error is left untouched. */
        NSError *deactivateError = nil;
        if (![session setActive:NO
                    withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation
                          error:&deactivateError]) {
            NSLog(@"gp32emu ios: audio session deactivation failed after an engine start failure: %@",
                  deactivateError.localizedDescription);
        }
        _running = NO;
        return NO;
    }
    _running = YES;
    return YES;
}

- (void)pauseForInterruption {
    _running = NO;
    if (_engine.isRunning) [_engine stop];
    if (_sink) gp32_ios_audio_sink_request_flush(_sink);
}

- (void)stop {
    _running = NO;
    if (_engine.isRunning) [_engine stop];
    if (_sink) gp32_ios_audio_sink_request_flush(_sink);
    NSError *error = nil;
    if (![AVAudioSession.sharedInstance setActive:NO
                                      withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation
                                            error:&error]) {
        NSLog(@"gp32emu ios: audio session deactivation failed: %@", error.localizedDescription);
    }
}

@end
