// Ours (MIT). See iceland-camera-device.h.
#import "iceland-camera-device.h"

#import "iceland-camera-log.h"
#import "iceland-camera-stream.h"

// kIOAudioDeviceTransportTypeBuiltIn, 'bltn': the value the documented CMIOExtensionPropertyDeviceTransportType carries.
// Spelled out rather than taken from IOKit's legacy audio header, which is where it is defined and also where the
// deprecation warnings live.
static const uint32_t kIcelandCameraTransportBuiltIn = 0x626C746E;

static NSString *const kIcelandCameraDeviceModel = @"OnePlus Pad 4 front camera";

// Ours, fixed for good: the device ID a client remembers, and the stream ID inside it.
static NSString *const kIcelandCameraDeviceUUID = @"6E1C4B4A-7E52-4F0B-9C3D-2A1F5B7C9D01";
static NSString *const kIcelandCameraStreamUUID = @"B7D3E2A1-4C68-4F92-8E15-3A9C6D0F2B74";

@implementation IcelandCameraDeviceSource

- (instancetype)initWithPatternMode:(BOOL)patternMode
{
    self = [super init];
    if (self) {
        NSUUID *deviceID = [[NSUUID alloc] initWithUUIDString:kIcelandCameraDeviceUUID];
        NSUUID *streamID = [[NSUUID alloc] initWithUUIDString:kIcelandCameraStreamUUID];
        NSError *error = nil;

        _streamSource = [[IcelandCameraStreamSource alloc] initWithPatternMode:patternMode];
        if (!_streamSource)
            return nil;

        _device = [CMIOExtensionDevice deviceWithLocalizedName:@"Front Camera"
                                                     deviceID:deviceID
                                                legacyDeviceID:nil
                                                       source:self];
        if (!_device)
            return nil;

        CMIOExtensionStream *stream = [CMIOExtensionStream streamWithLocalizedName:@"Front Camera"
                                                                          streamID:streamID
                                                                         direction:CMIOExtensionStreamDirectionSource
                                                                         clockType:CMIOExtensionStreamClockTypeHostTime
                                                                            source:_streamSource];
        if (!stream)
            return nil;
        _streamSource.stream = stream;
        if (![_device addStream:stream error:&error])
            iceland_camera_log("start: addStream failed: %s", iceland_camera_error_text(error));
    }
    return self;
}

#pragma mark - CMIOExtensionDeviceSource

- (NSSet<CMIOExtensionProperty> *)availableProperties
{
    return [NSSet setWithObjects:CMIOExtensionPropertyDeviceModel, CMIOExtensionPropertyDeviceTransportType, nil];
}

- (CMIOExtensionDeviceProperties *)devicePropertiesForProperties:(NSSet<CMIOExtensionProperty> *)properties
                                                           error:(NSError **)outError
{
    // The dictionary form wants CMIOExtensionPropertyState values; the typed accessors make those themselves.
    CMIOExtensionDeviceProperties *answer = [CMIOExtensionDeviceProperties devicePropertiesWithDictionary:@{}];

    (void)outError;
    if ([properties containsObject:CMIOExtensionPropertyDeviceModel])
        answer.model = kIcelandCameraDeviceModel;
    if ([properties containsObject:CMIOExtensionPropertyDeviceTransportType])
        answer.transportType = @(kIcelandCameraTransportBuiltIn);
    return answer;
}

- (BOOL)setDeviceProperties:(CMIOExtensionDeviceProperties *)deviceProperties error:(NSError **)outError
{
    // Nothing on the device is writable: the model, the transport and the identity are the hardware's.
    (void)deviceProperties;
    (void)outError;
    return YES;
}

@end
