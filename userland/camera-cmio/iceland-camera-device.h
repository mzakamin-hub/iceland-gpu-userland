// Ours (MIT). The device source of IcelandCamera: one camera, "Front Camera", with a fixed identity (the device and
// stream IDs are ours and never change, so a client's saved choice of this camera stays the same camera) and one stream.
#import <CoreMediaIO/CMIOExtension.h>
#import <Foundation/Foundation.h>

@class IcelandCameraStreamSource;

@interface IcelandCameraDeviceSource : NSObject <CMIOExtensionDeviceSource>

@property (nonatomic, readonly, strong) CMIOExtensionDevice *device;
@property (nonatomic, readonly, strong) IcelandCameraStreamSource *streamSource;

- (instancetype)initWithPatternMode:(BOOL)patternMode;

@end
