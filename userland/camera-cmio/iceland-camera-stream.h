// Ours (MIT). The stream source of IcelandCamera: one format (816 x 612, 32BGRA, 1/30 s frames, host-time clock), a
// frame pump on a serial queue, and the rules the CMIOExtension stream asks of its source (start, stop, properties,
// authorization).
#import <CoreMediaIO/CMIOExtension.h>
#import <Foundation/Foundation.h>

@interface IcelandCameraStreamSource : NSObject <CMIOExtensionStreamSource>

// Set by the device after it made the stream: the stream holds its source, not the other way round.
@property (nonatomic, weak) CMIOExtensionStream *stream;

// `patternMode` is the mode main.m chose for the provider; start falls back to it when the kernel service cannot be used.
- (instancetype)initWithPatternMode:(BOOL)patternMode;

@end
