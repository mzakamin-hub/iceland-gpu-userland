// Ours (MIT). See iceland-camera-stream.h. The pump runs on one serial queue; the CMIOExtension asks for start and stop on
// its client queue, so those two hand the work to the pump queue and wait for it. Two producers:
//
//   kernel   IOKit user client of the in-kernel IcelandCamera service: wait, convert (raw10.c), send;
//   pattern  a dispatch timer at 30 Hz that fills a colour-bar picture (iceland-camera-pattern.c) and sends it.
//
// Every picture leaves as a CVPixelBuffer from a pool (IOSurface-backed, as CMIO wants for a stream), described by a
// CMVideoFormatDescription made from that buffer, and wrapped in a CMSampleBuffer whose presentation time is the host
// clock's now and whose duration is 1/30 s.
#import "iceland-camera-stream.h"

#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>

#include <sys/types.h>
#include <time.h>

#import "iceland-camera-kernel.h"
#import "iceland-camera-log.h"
#import "iceland-camera-pattern.h"
#include "raw10.h"

static const int32_t kIcelandCameraWidth = ICELAND_RAW10_OUTPUT_WIDTH;
static const int32_t kIcelandCameraHeight = ICELAND_RAW10_OUTPUT_HEIGHT;
static const int32_t kIcelandCameraFrameRate = 30;
static const uint64_t kIcelandCameraWaitTimeoutMS = 500;
static const uint64_t kIcelandCameraLogEvery = 150;      // a line every five seconds of pictures

static NSError *iceland_camera_stream_error(NSInteger code, NSString *description)
{
    return [NSError errorWithDomain:@"org.iceland.camera" code:code
                           userInfo:@{NSLocalizedDescriptionKey : description}];
}

@interface IcelandCameraStreamSource () {
    dispatch_queue_t _queue;
    CMVideoFormatDescriptionRef _formatDescription;
    CMIOExtensionStreamFormat *_streamFormat;
    CVPixelBufferPoolRef _pool;
    iceland_camera_kernel _kernel;
    dispatch_source_t _timer;
    uint64_t _served;
    uint64_t _torn;
    uint32_t _patternFrame;
    NSString *_failureReason;
    BOOL _kernelOpen;
    BOOL _streaming;
    BOOL _patternMode;
    BOOL _reportedFailure;
}

- (BOOL)startOnQueue;
- (void)stopOnQueue;
- (void)pumpKernel;
- (void)pumpPattern;
- (void)sendPixelBuffer:(CVPixelBufferRef)pixelBuffer sequence:(uint64_t)sequence;
- (void)noteFailure:(NSString *)reason;

@end

@implementation IcelandCameraStreamSource

- (instancetype)initWithPatternMode:(BOOL)patternMode
{
    self = [super init];
    if (self) {
        CMTime duration = CMTimeMake(1, kIcelandCameraFrameRate);
        OSStatus status;

        _queue = dispatch_queue_create("org.iceland.camera.frames", DISPATCH_QUEUE_SERIAL);
        _patternMode = patternMode;

        // One format, and its description: 816 x 612 of 32BGRA, which is what the converter writes.
        status = CMVideoFormatDescriptionCreate(kCFAllocatorDefault, (CMVideoCodecType)kCVPixelFormatType_32BGRA,
                                                kIcelandCameraWidth, kIcelandCameraHeight, NULL, &_formatDescription);
        if (status != noErr || !_formatDescription) {
            iceland_camera_log("start: no format description (%d)", (int)status);
            return nil;
        }
        // validFrameDurations nil: the one duration, given as both the longest and the shortest.
        _streamFormat = [CMIOExtensionStreamFormat streamFormatWithFormatDescription:_formatDescription
                                                                    maxFrameDuration:duration
                                                                    minFrameDuration:duration
                                                                  validFrameDurations:nil];
        if (!_streamFormat)
            return nil;
    }
    return self;
}

- (void)dealloc
{
    if (_timer)
        dispatch_source_cancel(_timer);
    if (_kernelOpen) {
        iceland_camera_kernel_stop(&_kernel);
        iceland_camera_kernel_unmap(&_kernel);
        iceland_camera_kernel_close(&_kernel);
    }
    if (_pool)
        CVPixelBufferPoolRelease(_pool);
    if (_formatDescription)
        CFRelease(_formatDescription);
}

#pragma mark - CMIOExtensionStreamSource

- (NSArray<CMIOExtensionStreamFormat *> *)formats
{
    return @[ _streamFormat ];
}

- (NSSet<CMIOExtensionProperty> *)availableProperties
{
    return [NSSet setWithObjects:CMIOExtensionPropertyStreamActiveFormatIndex, CMIOExtensionPropertyStreamFrameDuration, nil];
}

- (CMIOExtensionStreamProperties *)streamPropertiesForProperties:(NSSet<CMIOExtensionProperty> *)properties
                                                            error:(NSError **)outError
{
    // The dictionary form wants CMIOExtensionPropertyState values; the typed accessors make those themselves (the
    // index an NSNumber, the duration a CMTime as its dictionary).
    CMIOExtensionStreamProperties *answer = [CMIOExtensionStreamProperties streamPropertiesWithDictionary:@{}];

    (void)outError;
    if ([properties containsObject:CMIOExtensionPropertyStreamActiveFormatIndex])
        answer.activeFormatIndex = @0;
    if ([properties containsObject:CMIOExtensionPropertyStreamFrameDuration]) {
        answer.frameDuration =
            CFBridgingRelease(CMTimeCopyAsDictionary(CMTimeMake(1, kIcelandCameraFrameRate), kCFAllocatorDefault));
    }
    return answer;
}

- (BOOL)setStreamProperties:(CMIOExtensionStreamProperties *)streamProperties error:(NSError **)outError
{
    NSNumber *index = streamProperties.activeFormatIndex;
    NSDictionary *duration = streamProperties.frameDuration;

    if (index && index.integerValue != 0) {
        if (outError)
            *outError = iceland_camera_stream_error(1, @"the stream has one format only (index 0)");
        return NO;
    }
    if (duration && CMTimeCompare(CMTimeMakeFromDictionary((__bridge CFDictionaryRef)duration),
                                  CMTimeMake(1, kIcelandCameraFrameRate)) != 0) {
        if (outError)
            *outError = iceland_camera_stream_error(2, @"the frame duration is fixed at 1/30 s");
        return NO;
    }
    return YES;
}

- (BOOL)authorizedToStartStreamForClient:(CMIOExtensionClient *)client
{
    iceland_camera_log("client pid %d authorized", (int)client.pid);
    iceland_camera_note(ICELAND_CAMERA_NOTE_CLIENT, (uint64_t)client.pid, 0);
    return YES;
}

- (BOOL)startStreamAndReturnError:(NSError **)outError
{
    __block BOOL started = NO;

    dispatch_sync(_queue, ^{
        started = [self startOnQueue];
    });
    if (!started) {
        NSString *reason = _failureReason;
        iceland_camera_log("stream start failed: %s", iceland_camera_text(reason));
        iceland_camera_note(ICELAND_CAMERA_NOTE_FAILURE, 1, 0);
        if (outError)
            *outError = iceland_camera_stream_error(3, reason ? reason : @"the stream did not start");
        return NO;
    }
    iceland_camera_log("stream start (mode %s)", _patternMode ? "pattern" : "kernel");
    iceland_camera_note(ICELAND_CAMERA_NOTE_STREAM_START, _patternMode ? 1 : 0, 0);
    return YES;
}

- (BOOL)stopStreamAndReturnError:(NSError **)outError
{
    __block BOOL wasStreaming = NO;
    __block uint64_t served = 0;
    __block uint64_t torn = 0;

    (void)outError;
    dispatch_sync(_queue, ^{
        wasStreaming = self->_streaming;
        served = self->_served;
        torn = self->_torn;
        [self stopOnQueue];
    });
    if (wasStreaming) {
        iceland_camera_log("stream stop: frames served %llu, torn %llu", (unsigned long long)served, (unsigned long long)torn);
        iceland_camera_note(ICELAND_CAMERA_NOTE_STREAM_STOP, served, torn);
    }
    return YES;
}

#pragma mark - Starting and stopping

- (BOOL)startOnQueue
{
    if (_streaming)
        return YES;

    if (!_pool) {
        NSDictionary *poolAttributes = @{ (id)kCVPixelBufferPoolMinimumBufferCountKey : @3 };
        NSDictionary *bufferAttributes = @{
            (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
            (id)kCVPixelBufferWidthKey : @(kIcelandCameraWidth),
            (id)kCVPixelBufferHeightKey : @(kIcelandCameraHeight),
            (id)kCVPixelBufferIOSurfacePropertiesKey : @{},
        };
        CVReturn created = CVPixelBufferPoolCreate(kCFAllocatorDefault, (__bridge CFDictionaryRef)poolAttributes,
                                                   (__bridge CFDictionaryRef)bufferAttributes, &_pool);
        if (created != kCVReturnSuccess || !_pool) {
            _failureReason = @"no pixel buffer pool";
            return NO;
        }
    }

    _served = 0;
    _torn = 0;
    _patternFrame = 0;
    _reportedFailure = NO;

    if (!_patternMode) {
        iceland_camera_kernel_info info;
        BOOL ready = NO;

        if (iceland_camera_kernel_open(&_kernel) == 0) {
            BOOL mapped = NO;
            BOOL running = NO;

            if (iceland_camera_kernel_get_info(&_kernel, &info) == 0 && info.bytes_per_buffer > 0 && info.buffer_count > 0 &&
                iceland_camera_kernel_map(&_kernel, info.bytes_per_buffer, info.buffer_count) == 0) {
                mapped = YES;
                if (iceland_camera_kernel_start(&_kernel) == 0)
                    running = YES;
            }
            if (mapped && running) {
                ready = YES;
            } else {
                if (running)
                    iceland_camera_kernel_stop(&_kernel);
                if (mapped)
                    iceland_camera_kernel_unmap(&_kernel);
                iceland_camera_kernel_close(&_kernel);
            }
        }
        if (ready) {
            _kernelOpen = YES;
            _kernel.last_sequence = 0;
        } else {
            _patternMode = YES;
            iceland_camera_log("start: kernel service not usable, pattern mode");
        }
    }

    _streaming = YES;
    if (_patternMode) {
        _timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, _queue);
        if (!_timer) {
            _streaming = NO;
            _failureReason = @"no timer";
            return NO;
        }
        dispatch_source_set_timer(_timer, dispatch_time(DISPATCH_TIME_NOW, 0), NSEC_PER_SEC / kIcelandCameraFrameRate,
                                  NSEC_PER_MSEC);
        __weak IcelandCameraStreamSource *weakSelf = self;
        dispatch_source_set_event_handler(_timer, ^{
            [weakSelf pumpPattern];
        });
        dispatch_resume(_timer);
    } else {
        dispatch_async(_queue, ^{
            [self pumpKernel];
        });
    }
    return YES;
}

- (void)stopOnQueue
{
    _streaming = NO;
    if (_timer) {
        dispatch_source_cancel(_timer);
        _timer = nil;
    }
    if (_kernelOpen) {
        iceland_camera_kernel_stop(&_kernel);
        iceland_camera_kernel_unmap(&_kernel);
        iceland_camera_kernel_close(&_kernel);
        _kernelOpen = NO;
    }
}

#pragma mark - Producing pictures

- (void)pumpKernel
{
    if (!_streaming || _patternMode)
        return;

    iceland_camera_kernel_frame frame;
    int waited = iceland_camera_kernel_wait(&_kernel, (uint32_t)kIcelandCameraWaitTimeoutMS, &frame);

    if (waited == ICELAND_CAMERA_KERNEL_TIMEOUT) {
        dispatch_async(_queue, ^{
            [self pumpKernel];
        });
        return;
    }
    if (waited != ICELAND_CAMERA_KERNEL_FRAME) {
        [self noteFailure:[NSString stringWithFormat:@"the kernel's wait answered %d", waited]];
        return;                                  // the client's stop still logs "CMIO stream stop"
    }
    if (frame.width != ICELAND_RAW10_SENSOR_WIDTH || frame.height != ICELAND_RAW10_SENSOR_HEIGHT ||
        frame.stride < (uint64_t)(ICELAND_RAW10_SENSOR_WIDTH / 4 * ICELAND_RAW10_GROUP_BYTES)) {
        [self noteFailure:@"a frame of an unexpected geometry"];
    } else {
        const uint8_t *raw = iceland_camera_kernel_buffer(&_kernel, frame.index);
        CVPixelBufferRef pixelBuffer = NULL;

        if (!raw) {
            [self noteFailure:@"a frame index outside the mapping"];
        } else if (CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, _pool, &pixelBuffer) != kCVReturnSuccess ||
                   !pixelBuffer) {
            [self noteFailure:@"no pixel buffer"];
        } else {
            CVPixelBufferLockBaseAddress(pixelBuffer, 0);
            iceland_raw10_to_bgra(raw, (size_t)frame.stride, CVPixelBufferGetBaseAddress(pixelBuffer),
                                  CVPixelBufferGetBytesPerRow(pixelBuffer));
            CVPixelBufferUnlockBaseAddress(pixelBuffer, 0);
            // The driver writes this buffer again two frames on: a picture converted too late is not sent.
            if (iceland_camera_kernel_torn(&_kernel, &frame))
                _torn++;
            else
                [self sendPixelBuffer:pixelBuffer sequence:frame.sequence];
            CVPixelBufferRelease(pixelBuffer);
        }
    }

    dispatch_async(_queue, ^{
        [self pumpKernel];
    });
}

- (void)pumpPattern
{
    CVPixelBufferRef pixelBuffer = NULL;

    if (!_streaming)
        return;
    if (CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, _pool, &pixelBuffer) != kCVReturnSuccess || !pixelBuffer) {
        [self noteFailure:@"no pixel buffer"];
        return;
    }
    CVPixelBufferLockBaseAddress(pixelBuffer, 0);
    iceland_camera_pattern_fill(CVPixelBufferGetBaseAddress(pixelBuffer), CVPixelBufferGetBytesPerRow(pixelBuffer),
                                kIcelandCameraWidth, kIcelandCameraHeight, _patternFrame);
    CVPixelBufferUnlockBaseAddress(pixelBuffer, 0);
    _patternFrame++;
    [self sendPixelBuffer:pixelBuffer sequence:_patternFrame];
    CVPixelBufferRelease(pixelBuffer);
}

- (void)sendPixelBuffer:(CVPixelBufferRef)pixelBuffer sequence:(uint64_t)sequence
{
    CMVideoFormatDescriptionRef description = NULL;
    CMSampleBufferRef sampleBuffer = NULL;
    CMSampleTimingInfo timing;
    CMIOExtensionStream *stream = self.stream;
    OSStatus status;

    if (!stream) {
        [self noteFailure:@"no stream to send to"];
        return;
    }

    status = CMVideoFormatDescriptionCreateForImageBuffer(kCFAllocatorDefault, pixelBuffer, &description);
    if (status != noErr || !description) {
        [self noteFailure:[NSString stringWithFormat:@"no format description for a picture (%d)", (int)status]];
        return;
    }

    timing.duration = CMTimeMake(1, kIcelandCameraFrameRate);
    timing.presentationTimeStamp = CMClockGetTime(CMClockGetHostTimeClock());
    timing.decodeTimeStamp = kCMTimeInvalid;

    status = CMSampleBufferCreateReadyWithImageBuffer(kCFAllocatorDefault, pixelBuffer, description, &timing, &sampleBuffer);
    CFRelease(description);
    if (status != noErr || !sampleBuffer) {
        [self noteFailure:[NSString stringWithFormat:@"no sample buffer (%d)", (int)status]];
        return;
    }

    // The sender returns nothing: a picture no client takes is dropped by the stream itself.
    [stream sendSampleBuffer:sampleBuffer
               discontinuity:CMIOExtensionStreamDiscontinuityFlagNone
       hostTimeInNanoseconds:clock_gettime_nsec_np(CLOCK_UPTIME_RAW)];
    CFRelease(sampleBuffer);

    _served++;
    if (_served == 1 || _served % kIcelandCameraLogEvery == 0) {
        iceland_camera_log("frames served %llu (kernel seq %llu, torn %llu)", (unsigned long long)_served,
                           (unsigned long long)sequence, (unsigned long long)_torn);
        iceland_camera_note(ICELAND_CAMERA_NOTE_FRAMES, _served, sequence);
    }
}

- (void)noteFailure:(NSString *)reason
{
    if (_reportedFailure)
        return;
    _reportedFailure = YES;
    iceland_camera_log("stream: %s", iceland_camera_text(reason));
    iceland_camera_note(ICELAND_CAMERA_NOTE_FAILURE, 2, _served);
}

@end
